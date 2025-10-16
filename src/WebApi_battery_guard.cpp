#include "WebApi_battery_guard.h"
#include "Configuration.h"
#include "BatteryGuard.h"
#include "defaults.h"
#include "WebApi.h"
#include <AsyncJson.h>
#include <battery/Controller.h>

void WebApiBatteryGuardClass::init(AsyncWebServer& server, Scheduler& scheduler)
{
    using std::placeholders::_1;

    _server = &server;

    _server->on("/api/batteryguard/status", HTTP_GET, std::bind(&WebApiBatteryGuardClass::onStatus, this, _1));
    _server->on("/api/batteryguard/config", HTTP_GET, std::bind(&WebApiBatteryGuardClass::onAdminGet, this, _1));
    _server->on("/api/batteryguard/config", HTTP_POST, std::bind(&WebApiBatteryGuardClass::onAdminPost, this, _1));
    _server->on("/api/batteryguard/metadata", HTTP_GET, std::bind(&WebApiBatteryGuardClass::onMetaData, this, _1));
}

void WebApiBatteryGuardClass::onStatus(AsyncWebServerRequest* request)
{
    if (!WebApi.checkCredentialsReadonly(request)) {
        return;
    }

    AsyncJsonResponse* response = new AsyncJsonResponse();
    auto root = response->getRoot().as<JsonObject>();

    BatteryGuard.serializeInfo(root);

    WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
}

void WebApiBatteryGuardClass::onMetaData(AsyncWebServerRequest* request)
{
    if (!WebApi.checkCredentials(request)) { return; }

    auto const& config = Configuration.get();

    AsyncJsonResponse* response = new AsyncJsonResponse();
    auto& root = response->getRoot();

    root["battery_enabled"] = config.Battery.Enabled;

    WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
}

void WebApiBatteryGuardClass::onAdminGet(AsyncWebServerRequest* request)
{
    if (!WebApi.checkCredentials(request)) { return; }

    AsyncJsonResponse* response = new AsyncJsonResponse();
    auto root = response->getRoot().as<JsonObject>();
    auto const& config = Configuration.get();
    ConfigurationClass::serializeBatteryGuardConfig(config.BatteryGuard, config.PowerLimiter, root);
    WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
}

void WebApiBatteryGuardClass::onAdminPost(AsyncWebServerRequest* request)
{
    if (!WebApi.checkCredentials(request)) { return; }

    AsyncJsonResponse* response = new AsyncJsonResponse();
    JsonDocument root;
    if (!WebApi.parseRequestData(request, response, root)) {
        return;
    }

    auto& retMsg = response->getRoot();

    {
        auto guard = Configuration.getWriteGuard();
        auto& config = guard.getConfig();
        ConfigurationClass::deserializeBatteryGuardConfig(root.as<JsonObject>(), config.BatteryGuard);
    }

    WebApi.writeConfig(retMsg);

    response->setLength();
    request->send(response);

    BatteryGuard.updateSettings(BatteryGuardClass::UpdateSource::BATTERY_GUARD);
}
