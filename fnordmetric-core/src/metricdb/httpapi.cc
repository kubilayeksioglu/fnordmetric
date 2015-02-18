/**
 * This file is part of the "FnordMetric" project
 *   Copyright (c) 2014 Paul Asmuth, Google Inc.
 *
 * FnordMetric is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License v3.0. You should have received a
 * copy of the GNU General Public License along with this program. If not, see
 * <http://www.gnu.org/licenses/>.
 */
#include <fnordmetric/environment.h>
#include <fnordmetric/metricdb/httpapi.h>
#include <fnordmetric/query/queryservice.h>
#include <fnordmetric/metricdb/metricrepository.h>
#include <fnordmetric/metricdb/metrictablerepository.h>
#include <fnordmetric/util/stringutil.h>
#include <fnordmetric/sql/backends/csv/csvbackend.h>
#include <fnordmetric/sql/backends/mysql/mysqlbackend.h>

#include <boost/lexical_cast.hpp>
#include <iostream>

namespace fnordmetric {
namespace metricdb {

static const char kMetricsUrl[] = "/metrics";
static const char kMetricsUrlPrefix[] = "/metrics/";
static const char kQueryUrl[] = "/query";
static const char kLabelParamPrefix[] = "label[";

HTTPAPI::HTTPAPI(IMetricRepository* metric_repo) : metric_repo_(metric_repo) {}

bool HTTPAPI::handleHTTPRequest(
    http::HTTPRequest* request,
    http::HTTPResponse* response) {
  util::URI uri(request->getUrl());
  auto path = uri.path();
  fnord::util::StringUtil::stripTrailingSlashes(&path);

  response->addHeader("Access-Control-Allow-Origin", "*");

  // PATH: ^/metrics/?$
  if (path == kMetricsUrl) {
    switch (request->method()) {
      case http::HTTPRequest::M_GET:
        renderMetricList(request, response, &uri);
        return true;
      case http::HTTPRequest::M_POST:
        insertSample(request, response, &uri);
        return true;
      default:
        return false;
    }
  }

  // PATH: ^/metrics/.*
  if (path.compare(0, sizeof(kMetricsUrlPrefix) - 1, kMetricsUrlPrefix) == 0) {
    // PATH: ^/metrics/(.*)$
    switch (request->method()) {
      case http::HTTPRequest::M_GET:
        renderMetricSampleScan(request, response, &uri);
        return true;
      default:
        return false;
    }
  }

  // PATH: ^/query/?*
  if (path == kQueryUrl) {
    switch (request->method()) {
      case http::HTTPRequest::M_GET:
      case http::HTTPRequest::M_POST:
        executeQuery(request, response, &uri);
        return true;
      default:
        return false;
    }
    return true;
  }

  return false;
}

void HTTPAPI::renderMetricList(
    http::HTTPRequest* request,
    http::HTTPResponse* response,
    util::URI* uri) {
  response->setStatus(http::kStatusOK);
  response->addHeader("Content-Type", "application/json; charset=utf-8");
  util::JSONOutputStream json(response->getBodyOutputStream());

  json.beginObject();
  json.addObjectEntry("metrics");
  json.beginArray();

  int i = 0;
  for (const auto& metric : metric_repo_->listMetrics()) {
    if (i++ > 0) { json.addComma(); }
    renderMetricJSON(metric, &json);
  }

  json.endArray();
  json.endObject();
}

void HTTPAPI::insertSample(
    http::HTTPRequest* request,
    http::HTTPResponse* response,
    util::URI* uri) {
  const auto& postbody = request->getBody();
  util::URI::ParamList params;

  if (postbody.size() > 0) {
    util::URI::parseQueryString(postbody, &params);
  } else {
    params = uri->queryParams();
  }

  std::string metric_key;
  if (!util::URI::getParam(params, "metric", &metric_key)) {
    response->addBody("error: invalid metric key: " + metric_key);
    response->setStatus(http::kStatusBadRequest);
    return;
  }

  std::string value_str;
  if (!util::URI::getParam(params, "value", &value_str)) {
    response->addBody("error: missing ?value=... parameter");
    response->setStatus(http::kStatusBadRequest);
    return;
  }

  std::vector<std::pair<std::string, std::string>> labels;
  for (const auto& param : params) {
    const auto& key = param.first;
    const auto& value = param.second;

    if (key.compare(0, sizeof(kLabelParamPrefix) - 1, kLabelParamPrefix) == 0 &&
        key.back() == ']') {
      auto label_key = key.substr(
          sizeof(kLabelParamPrefix) - 1,
          key.size() - sizeof(kLabelParamPrefix));

      labels.emplace_back(label_key, value);
    }
  }

  double sample_value;
  try {
    sample_value = std::stod(value_str);
  } catch (std::exception& e) {
    response->addBody("error: invalid value: " + value_str);
    response->setStatus(http::kStatusBadRequest);
    return;
  }

  auto metric = metric_repo_->findOrCreateMetric(metric_key);

  // get optional timestap from user & pass to insert sample
  uint64_t timestamp = 0;
  std::string timestamp_str;
  if(util::URI::getParam(params, "timestamp", &timestamp_str)){
    // str to timestamp
    timestamp = boost::lexical_cast<uint64_t>(timestamp_str);

    env()->logger()->printf(
        "DEBUG",
        "timestamp: %s",
        timestamp_str.c_str());

    env()->logger()->printf(
        "DEBUG",
        "timestamp: %llu",
        (long long unsigned) timestamp);
  }

  env()->logger()->printf("DEBUG", "TEST");

  metric->insertSample(sample_value, labels, timestamp);
  response->setStatus(http::kStatusCreated);
}

void HTTPAPI::renderMetricSampleScan(
    http::HTTPRequest* request,
    http::HTTPResponse* response,
    util::URI* uri) {
  auto metric_key = uri->path().substr(sizeof(kMetricsUrlPrefix) - 1);
  if (metric_key.size() < 3) {
    response->addBody("error: invalid metric key: " + metric_key);
    response->setStatus(http::kStatusBadRequest);
    return;
  }

  auto metric = metric_repo_->findMetric(metric_key);
  if (metric == nullptr) {
    response->addBody("metric not found: " + metric_key);
    response->setStatus(http::kStatusNotFound);
    return;
  }

  response->setStatus(http::kStatusOK);
  response->addHeader("Content-Type", "application/json; charset=utf-8");
  util::JSONOutputStream json(response->getBodyOutputStream());

  json.beginObject();

  json.addObjectEntry("metric");
  renderMetricJSON(metric, &json);
  json.addComma();

  json.addObjectEntry("samples");
  json.beginArray();

  int i = 0;
  metric->scanSamples(
      fnord::util::DateTime::epoch(),
      fnord::util::DateTime::now(),
      [&json, &i] (Sample* sample) -> bool {
        if (i++ > 0) { json.addComma(); }
        json.beginObject();

        json.addObjectEntry("time");
        json.addLiteral<uint64_t>(static_cast<uint64_t>(sample->time()));
        json.addComma();

        json.addObjectEntry("value");
        json.addLiteral<double>(sample->value());
        json.addComma();

        json.addObjectEntry("labels");
        json.beginObject();
        auto labels = sample->labels();
        for (int n = 0; n < labels.size(); n++) {
          if (n > 0) {
            json.addComma();
          }

          json.addObjectEntry(labels[n].first);
          json.addString(labels[n].second);
        }
        json.endObject();

        json.endObject();
        return true;
      });

  json.endArray();
  json.endObject();
}

void HTTPAPI::executeQuery(
    http::HTTPRequest* request,
    http::HTTPResponse* response,
    util::URI* uri) {
  auto params = uri->queryParams();

  std::shared_ptr<util::InputStream> input_stream;
  std::string get_query;
  if (util::URI::getParam(params, "q", &get_query)) {
    input_stream.reset(new util::StringInputStream(get_query));
  } else {
    input_stream = request->getBodyInputStream();
  }

  std::shared_ptr<util::OutputStream> output_stream =
      response->getBodyOutputStream();

  query::QueryService query_service;
  std::unique_ptr<query::TableRepository> table_repo(
      new MetricTableRepository(metric_repo_));


  if (!env()->flags()->isSet("disable_external_sources")) {
    query_service.registerBackend(
        std::unique_ptr<fnordmetric::query::Backend>(
            new fnordmetric::query::mysql_backend::MySQLBackend));

    query_service.registerBackend(
        std::unique_ptr<fnordmetric::query::Backend>(
            new fnordmetric::query::csv_backend::CSVBackend));
  }

  query::QueryService::kFormat resp_format = query::QueryService::FORMAT_JSON;
  std::string format_param;
  if (util::URI::getParam(params, "format", &format_param)) {
    if (format_param == "svg") {
      resp_format = query::QueryService::FORMAT_SVG;
    }
  }

  response->setStatus(http::kStatusOK);

  switch (resp_format) {
    case query::QueryService::FORMAT_JSON:
      response->addHeader("Content-Type", "application/json; charset=utf-8");
      break;
    case query::QueryService::FORMAT_SVG:
      response->addHeader("Content-Type", "text/html; charset=utf-8");
      break;
    default:
      break;
  }

  int width = -1;
  std::string width_param;
  if (util::URI::getParam(params, "width", &width_param)) {
    width = std::stoi(width_param);
  }

  int height = -1;
  std::string height_param;
  if (util::URI::getParam(params, "height", &height_param)) {
    height = std::stoi(height_param);
  }

  try {
    query_service.executeQuery(
        input_stream,
        resp_format,
        output_stream,
        std::move(table_repo),
        width,
        height);

  } catch (util::RuntimeException e) {
    response->clearBody();

    util::JSONOutputStream json(std::move(output_stream));
    json.beginObject();
    json.addObjectEntry("status");
    json.addString("error");
    json.addComma();
    json.addObjectEntry("error");
    json.addString(e.getMessage());
    json.endObject();
  }
}

void HTTPAPI::renderMetricJSON(
    IMetric* metric,
    util::JSONOutputStream* json) const {
  json->beginObject();

  json->addObjectEntry("key");
  json->addString(metric->key());
  json->addComma();

  json->addObjectEntry("total_bytes");
  json->addLiteral<size_t>(metric->totalBytes());
  json->addComma();

  json->addObjectEntry("last_insert");
  json->addLiteral<uint64_t>(static_cast<uint64_t>(metric->lastInsertTime()));
  json->addComma();

  json->addObjectEntry("labels");
  json->beginArray();
  auto labels = metric->labels();
  for (auto cur = labels.begin(); cur != labels.end(); ++cur) {
    if (cur != labels.begin()) {
      json->addComma();
    }
    json->addString(*cur);
  }
  json->endArray();

  json->endObject();
}

}
}
