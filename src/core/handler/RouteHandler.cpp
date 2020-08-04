#include "RouteHandler.hpp"

#include "common/QvHelpers.hpp"
#include "core/CoreUtils.hpp"
#include "core/connection/Generation.hpp"
#include "core/handler/ConfigHandler.hpp"
namespace Qv2ray::core::handler
{
    RouteHandler::RouteHandler(QObject *parent) : QObject(parent)
    {
        const auto routesJson = JsonFromString(StringFromFile(QV2RAY_CONFIG_DIR + "routes.json"));
        for (const auto &routeId : routesJson.keys())
        {
            configs.insert(GroupRoutingId{ routeId }, GroupRoutingConfig::fromJson(routesJson.value(routeId).toObject()));
        }
    }

    RouteHandler::~RouteHandler()
    {
        SaveRoutes();
    }

    void RouteHandler::SaveRoutes() const
    {
        QJsonObject routingObject;
        for (const auto &key : configs.keys())
        {
            routingObject[key.toString()] = configs[key].toJson();
        }
        StringToFile(JsonToString(routingObject), QV2RAY_CONFIG_DIR + "routes.json");
    }

    bool RouteHandler::SetDNSSettings(const GroupRoutingId &id, bool overrideGlobal, const QvConfig_DNS &dns)
    {
        configs[id].overrideDNS = overrideGlobal;
        configs[id].dnsConfig = dns;
        return true;
    }
    bool RouteHandler::SetAdvancedRouteSettings(const GroupRoutingId &id, bool overrideGlobal, const QvConfig_Route &route)
    {
        configs[id].overrideRoute = overrideGlobal;
        configs[id].routeConfig = route;
        return true;
    }

    // -------------------------- BEGIN CONFIG GENERATIONS
    ROUTING RouteHandler::GenerateRoutes(bool enableProxy, bool bypassCN, const QString &outTag, const QvConfig_Route &routeConfig) const
    {
        ROUTING root;
        root.insert("domainStrategy", routeConfig.domainStrategy);
        //
        // For Rules list
        QJsonArray rulesList;

        // Private IPs should always NOT TO PROXY!
        rulesList.append(GenerateSingleRouteRule(RULE_IP, "geoip:private", OUTBOUND_TAG_DIRECT));
        //
        if (!enableProxy)
        {
            // This is added to disable all proxies, as a alternative influence of #64
            rulesList.append(GenerateSingleRouteRule(RULE_DOMAIN, "regexp:.*", OUTBOUND_TAG_DIRECT));
            rulesList.append(GenerateSingleRouteRule(RULE_IP, "0.0.0.0/0", OUTBOUND_TAG_DIRECT));
            rulesList.append(GenerateSingleRouteRule(RULE_IP, "::/0", OUTBOUND_TAG_DIRECT));
        }
        else
        {
            //
            // Blocked.
            if (!routeConfig.ips.block.isEmpty())
            {
                rulesList.append(GenerateSingleRouteRule(RULE_IP, routeConfig.ips.block, OUTBOUND_TAG_BLACKHOLE));
            }
            if (!routeConfig.domains.block.isEmpty())
            {
                rulesList.append(GenerateSingleRouteRule(RULE_DOMAIN, routeConfig.domains.block, OUTBOUND_TAG_BLACKHOLE));
            }
            //
            // Proxied
            if (!routeConfig.ips.proxy.isEmpty())
            {
                rulesList.append(GenerateSingleRouteRule(RULE_IP, routeConfig.ips.proxy, outTag));
            }
            if (!routeConfig.domains.proxy.isEmpty())
            {
                rulesList.append(GenerateSingleRouteRule(RULE_DOMAIN, routeConfig.domains.proxy, outTag));
            }
            //
            // Directed
            if (!routeConfig.ips.direct.isEmpty())
            {
                rulesList.append(GenerateSingleRouteRule(RULE_IP, routeConfig.ips.direct, OUTBOUND_TAG_DIRECT));
            }
            if (!routeConfig.domains.direct.isEmpty())
            {
                rulesList.append(GenerateSingleRouteRule(RULE_DOMAIN, routeConfig.domains.direct, OUTBOUND_TAG_DIRECT));
            }
            //
            // Check if CN needs proxy, or direct.
            if (bypassCN)
            {
                // No proxy agains CN addresses.
                rulesList.append(GenerateSingleRouteRule(RULE_IP, "geoip:cn", OUTBOUND_TAG_DIRECT));
                rulesList.append(GenerateSingleRouteRule(RULE_IP, "geosite:cn", OUTBOUND_TAG_DIRECT));
            }
        }

        root.insert("rules", rulesList);
        return root;
    }
    // -------------------------- END CONFIG GENERATIONS
    //
    // BEGIN RUNTIME CONFIG GENERATION
    // We need copy construct here
    CONFIGROOT RouteHandler::GenerateFinalConfig(const ConnectionGroupPair &p, bool api) const
    {
        return GenerateFinalConfig(ConnectionManager->GetConnectionRoot(p.connectionId), ConnectionManager->GetGroupRoutingId(p.groupId), api);
    }
    CONFIGROOT RouteHandler::GenerateFinalConfig(CONFIGROOT root, const GroupRoutingId &routingId, bool hasAPI) const
    {
        const auto &config = configs.contains(routingId) ? configs[routingId] : GlobalConfig.defaultRouteConfig;
        //
        const auto &connConf = config.overrideConnectionConfig ? config.connectionConfig : GlobalConfig.defaultRouteConfig.connectionConfig;
        const auto &dnsConf = config.overrideDNS ? config.dnsConfig : GlobalConfig.defaultRouteConfig.dnsConfig;
        const auto &routeConf = config.overrideRoute ? config.routeConfig : GlobalConfig.defaultRouteConfig.routeConfig;
        const auto &fpConf = config.overrideForwardProxyConfig ? config.forwardProxyConfig : GlobalConfig.defaultRouteConfig.forwardProxyConfig;
        //
        //
        // Note: The part below always makes the whole functionality in
        // trouble...... BE EXTREME CAREFUL when changing these code
        // below...
        //
        // Check if is complex BEFORE adding anything.
        bool isComplex = IsComplexConfig(root);

        if (isComplex)
        {
            // For some config files that has routing entries already.
            // We DO NOT add extra routings.
            //
            // HOWEVER, we need to verify the QV2RAY_RULE_ENABLED entry.
            // And what's more, process (by removing unused items) from a
            // rule object.
            ROUTING routing(root["routing"].toObject());
            QJsonArray rules;
            LOG(MODULE_CONNECTION, "Processing an existing routing table.")

            for (const auto &_rule : routing["rules"].toArray())
            {
                auto rule = _rule.toObject();

                // For backward compatibility
                if (rule.contains("QV2RAY_RULE_USE_BALANCER"))
                {
                    // We use balancer, or the normal outbound
                    rule.remove(rule["QV2RAY_RULE_USE_BALANCER"].toBool(false) ? "outboundTag" : "balancerTag");
                }
                else
                {
                    LOG(MODULE_SETTINGS, "We found a rule without QV2RAY_RULE_USE_BALANCER, so didn't process it.")
                }

                // If this entry has been disabled.
                if (rule.contains("QV2RAY_RULE_ENABLED") && rule["QV2RAY_RULE_ENABLED"].toBool() == false)
                {
                    LOG(MODULE_SETTINGS, "Discarded a rule as it's been set DISABLED")
                }
                else
                {
                    rules.append(rule);
                }
            }

            routing["rules"] = rules;
            root["routing"] = routing;
        }
        else
        {
            LOG(MODULE_CONNECTION, "Inserting default values to simple config")
            if (root["outbounds"].toArray().count() != 1)
            {
                // There are no ROUTING but 2 or more outbounds.... This is rare, but possible.
                LOG(MODULE_CONNECTION, "WARN: This message usually indicates the config file has logic errors:")
                LOG(MODULE_CONNECTION, "WARN: --> The config file has NO routing section, however more than 1 outbounds are detected.")
            }
            //
            auto tag = QJsonIO::GetValue(root, "outbounds", 0, "tag").toString();
            if (tag.isEmpty())
            {
                LOG(MODULE_CONNECTION, "Applying workaround when an outbound tag is empty")
                tag = GenerateRandomString(15);
                QJsonIO::SetValue(root, tag, "outbounds", 0, "tag");
            }
            root["routing"] = GenerateRoutes(connConf.enableProxy, connConf.bypassCN, tag, routeConf);
            //
            // Forward proxy
            if (fpConf.enableForwardProxy)
            {
                auto outboundArray = root["outbounds"].toArray();
                auto firstOutbound = outboundArray.first().toObject();
                if (firstOutbound[QV2RAY_USE_FPROXY_KEY].toBool(false))
                {
                    LOG(MODULE_CONNECTION, "Applying forward proxy to current connection.")
                    firstOutbound["proxySettings"] = QJsonObject{ { "tag", OUTBOUND_TAG_FORWARD_PROXY } };

                    // Forward proxy.
                    if (fpConf.type.isEmpty())
                    {
                        DEBUG(MODULE_CONNECTION, "WARNING: Empty forward proxy type.")
                    }
                    else if (fpConf.type.toLower() != "http" && fpConf.type.toLower() != "socks")
                    {
                        DEBUG(MODULE_CONNECTION, "WARNING: Unsupported forward proxy type: " + fpConf.type)
                    }
                    else
                    {
                        const auto forwardProxySettings = GenerateHTTPSOCKSOut(fpConf.serverAddress, //
                                                                               fpConf.port,          //
                                                                               fpConf.useAuth,       //
                                                                               fpConf.username,      //
                                                                               fpConf.password);
                        const auto forwardProxyOutbound = GenerateOutboundEntry(OUTBOUND_TAG_FORWARD_PROXY, //
                                                                                fpConf.type.toLower(),      //
                                                                                forwardProxySettings, {});
                        outboundArray.push_back(forwardProxyOutbound);
                    }
                }
                else
                {
                    // Remove proxySettings from firstOutbound
                    firstOutbound.remove("proxySettings");
                }

                outboundArray.replace(0, firstOutbound);
                root["outbounds"] = outboundArray;
            }
            //
            // Process FREEDOM and BLACKHOLE outbound
            {
                OUTBOUNDS outbounds(root["outbounds"].toArray());
                const auto freeDS = (connConf.v2rayFreedomDNS) ? "UseIP" : "AsIs";
                outbounds.append(GenerateOutboundEntry(OUTBOUND_TAG_DIRECT, "freedom", GenerateFreedomOUT(freeDS, ":0", 0), {}));
                outbounds.append(GenerateOutboundEntry(OUTBOUND_TAG_BLACKHOLE, "blackhole", GenerateBlackHoleOUT(false), {}));
                root["outbounds"] = outbounds;
            }
            //
            // Connection Filters
            if (GlobalConfig.defaultRouteConfig.connectionConfig.dnsIntercept)
            {
                const auto hasTProxy = GlobalConfig.inboundConfig.useTPROXY && GlobalConfig.inboundConfig.tProxySettings.hasUDP;
                const auto hasIPv6 = hasTProxy && (!GlobalConfig.inboundConfig.tProxySettings.tProxyV6IP.isEmpty());
                const auto hasSocksUDP = GlobalConfig.inboundConfig.useSocks && GlobalConfig.inboundConfig.socksSettings.enableUDP;
                DNSInterceptFilter(root, hasTProxy, hasIPv6, hasSocksUDP);
            }

            if (GlobalConfig.inboundConfig.useTPROXY && GlobalConfig.outboundConfig.mark > 0)
                OutboundMarkSettingFilter(root, GlobalConfig.outboundConfig.mark);

            // Process bypass bitTorrent
            if (connConf.bypassBT)
                BypassBTFilter(root);

            // Process mKCP seed
            mKCPSeedFilter(root);

            // Remove empty Mux object from settings
            RemoveEmptyMuxFilter(root);
        }
        //
        // Process Log
        QJsonIO::SetValue(root, V2RayLogLevel[GlobalConfig.logLevel], "log", "loglevel");

        //
        // Process DNS
        const auto hasDNS = root.contains("dns") && !root.value("dns").toObject().isEmpty();
        if (!hasDNS)
        {
            root.insert("dns", GenerateDNS(connConf.withLocalDNS, dnsConf));
            LOG(MODULE_CONNECTION, "Added global DNS config")
        }

        //
        // If inbounds list is empty, we append our global configured inbounds to the config.
        // The setting applies to BOTH complex config AND simple config.
        // Just to ensure there's AT LEAST 1 possible inbound is being configured.
        if (!root.contains("inbounds") || root.value("inbounds").toArray().empty())
        {
            root["inbounds"] = GenerateDefaultInbounds();
            DEBUG(MODULE_CONNECTION, "Added global inbound config")
        }
        // API 0 speed issue when no tag is configured.
        // Process every inbounds to make sure a tag is configured, fixed
        FillupTagsFilter(root, "inbounds");

        //
        // Let's process some api features.
        if (hasAPI && GlobalConfig.kernelConfig.enableAPI)
        {
            //
            // Stats
            root.insert("stats", QJsonObject());
            //
            // Routes
            QJsonObject routing = root["routing"].toObject();
            QJsonArray routingRules = routing["rules"].toArray();
            const static QJsonObject APIRouteRoot{ { "type", "field" },                //
                                                   { "outboundTag", API_TAG_DEFAULT }, //
                                                   { "inboundTag", QJsonArray{ API_TAG_INBOUND } } };
            routingRules.push_front(APIRouteRoot);
            routing["rules"] = routingRules;
            root["routing"] = routing;
            //
            // Policy
            QJsonIO::SetValue(root, true, "policy", "system", "statsInboundUplink");
            QJsonIO::SetValue(root, true, "policy", "system", "statsInboundDownlink");
            QJsonIO::SetValue(root, true, "policy", "system", "statsOutboundUplink");
            QJsonIO::SetValue(root, true, "policy", "system", "statsOutboundDownlink");
            //
            // Inbounds
            INBOUNDS inbounds(root["inbounds"].toArray());
            QJsonObject fakeDocodemoDoor{ { "address", "127.0.0.1" } };
            const auto apiInboundsRoot = GenerateInboundEntry(API_TAG_INBOUND, "dokodemo-door",    //
                                                              "127.0.0.1",                         //
                                                              GlobalConfig.kernelConfig.statsPort, //
                                                              INBOUNDSETTING(fakeDocodemoDoor));
            inbounds.push_front(apiInboundsRoot);
            root["inbounds"] = inbounds;
            //
            // API
            root["api"] = GenerateAPIEntry(API_TAG_DEFAULT);
        }

        return root;
    }
} // namespace Qv2ray::core::handler
