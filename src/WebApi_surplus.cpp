#include <AsyncJson.h>
#include "WebApi.h"
#include "WebApi_surplus.h"
#include "SurplusPower.h"

void WebApiSurplusClass::init(AsyncWebServer& server, Scheduler& scheduler)
{
    using std::placeholders::_1;

    _server = &server;

    _server->on("/api/surplus/status", HTTP_GET,  static_cast<ArRequestHandlerFunction>(std::bind(&WebApiSurplusClass::onStatus, this, _1)));
}

void WebApiSurplusClass::onStatus(AsyncWebServerRequest* request)
{
    if (!WebApi.checkCredentialsReadonly(request)) {
        return;
    }

    AsyncJsonResponse* response = new AsyncJsonResponse();
    auto root = response->getRoot().as<JsonObject>();
    Surplus.serializeInfo(root);

    WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
}
