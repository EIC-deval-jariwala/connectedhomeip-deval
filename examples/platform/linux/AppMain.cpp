/*
 *
 *    Copyright (c) 2021-2022 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include <platform/CHIPDeviceLayer.h>
#include <platform/PlatformManager.h>

#include "app/clusters/network-commissioning/network-commissioning.h"
#include <app/server/OnboardingCodesUtil.h>
#include <app/server/Server.h>
#include <app/util/endpoint-config-api.h>
#include <crypto/CHIPCryptoPAL.h>
#include <lib/core/CHIPError.h>
#include <lib/core/NodeId.h>
#include <lib/core/Optional.h>
#include <lib/support/logging/CHIPLogging.h>

#include <credentials/DeviceAttestationCredsProvider.h>
#include <credentials/attestation_verifier/DefaultDeviceAttestationVerifier.h>
#include <credentials/attestation_verifier/DeviceAttestationVerifier.h>

#include <lib/support/CHIPMem.h>
#include <lib/support/ScopedBuffer.h>
#include <lib/support/TestGroupData.h>
#include <setup_payload/QRCodeSetupPayloadGenerator.h>
#include <setup_payload/SetupPayload.h>

#include <platform/CommissionableDataProvider.h>
#include <platform/DiagnosticDataProvider.h>
#include <platform/RuntimeOptionsProvider.h>

#include <DeviceInfoProviderImpl.h>

#if CHIP_DEVICE_CONFIG_ENABLE_BOTH_COMMISSIONER_AND_COMMISSIONEE
#include "CommissionerMain.h"
#include <ControllerShellCommands.h>
#include <controller/CHIPDeviceControllerFactory.h>
#include <controller/ExampleOperationalCredentialsIssuer.h>
#include <lib/core/CHIPPersistentStorageDelegate.h>
#include <platform/KeyValueStoreManager.h>
#endif // CHIP_DEVICE_CONFIG_ENABLE_BOTH_COMMISSIONER_AND_COMMISSIONEE

#if defined(ENABLE_CHIP_SHELL)
#include <CommissioneeShellCommands.h>
#include <lib/shell/Engine.h> // nogncheck
#include <thread>
#endif

#if defined(PW_RPC_ENABLED)
#include <Rpc.h>
#endif

#if CHIP_CONFIG_TRANSPORT_TRACE_ENABLED
#include "TraceDecoder.h"
#include "TraceHandlers.h"
#endif // CHIP_CONFIG_TRANSPORT_TRACE_ENABLED

#if ENABLE_TRACING
#include <TracingCommandLineArgument.h> // nogncheck
#endif

#if CHIP_DEVICE_CONFIG_ENABLE_OTA_REQUESTOR
#include <app/clusters/ota-requestor/OTATestEventTriggerDelegate.h>
#endif
#if CHIP_DEVICE_CONFIG_ENABLE_SMOKE_CO_TRIGGER
#include <app/clusters/smoke-co-alarm-server/SmokeCOTestEventTriggerDelegate.h>
#endif
#include <app/TestEventTriggerDelegate.h>

#include <signal.h>

#include "AppMain.h"
#include "CommissionableInit.h"

#if CHIP_DEVICE_LAYER_TARGET_DARWIN
#include <platform/Darwin/NetworkCommissioningDriver.h>
#if CHIP_DEVICE_CONFIG_ENABLE_WIFI
#include <platform/Darwin/WiFi/NetworkCommissioningWiFiDriver.h>
#endif // CHIP_DEVICE_CONFIG_ENABLE_WIFI
#endif // CHIP_DEVICE_LAYER_TARGET_DARWIN

#if CHIP_DEVICE_LAYER_TARGET_LINUX
#include <platform/Linux/NetworkCommissioningDriver.h>
#endif // CHIP_DEVICE_LAYER_TARGET_LINUX

using namespace chip;
using namespace chip::ArgParser;
using namespace chip::Credentials;
using namespace chip::DeviceLayer;
using namespace chip::Inet;
using namespace chip::Transport;
using namespace chip::app::Clusters;

// Network comissioning implementation
namespace {
// If secondaryNetworkCommissioningEndpoint has a value and both Thread and WiFi
// are enabled, we put the WiFi network commissioning cluster on
// secondaryNetworkCommissioningEndpoint.
Optional<EndpointId> sSecondaryNetworkCommissioningEndpoint;

#if CHIP_DEVICE_LAYER_TARGET_LINUX
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#define CHIP_APP_MAIN_HAS_THREAD_DRIVER 1
DeviceLayer::NetworkCommissioning::LinuxThreadDriver sThreadDriver;
#endif // CHIP_DEVICE_CONFIG_ENABLE_THREAD

#if CHIP_DEVICE_CONFIG_ENABLE_WIFI
#define CHIP_APP_MAIN_HAS_WIFI_DRIVER 1
DeviceLayer::NetworkCommissioning::LinuxWiFiDriver sWiFiDriver;
#endif // CHIP_DEVICE_CONFIG_ENABLE_WIFI

#define CHIP_APP_MAIN_HAS_ETHERNET_DRIVER 1
DeviceLayer::NetworkCommissioning::LinuxEthernetDriver sEthernetDriver;
#endif // CHIP_DEVICE_LAYER_TARGET_LINUX

#if CHIP_DEVICE_LAYER_TARGET_DARWIN
#if CHIP_DEVICE_CONFIG_ENABLE_WIFI
#define CHIP_APP_MAIN_HAS_WIFI_DRIVER 1
DeviceLayer::NetworkCommissioning::DarwinWiFiDriver sWiFiDriver;
#endif // CHIP_DEVICE_CONFIG_ENABLE_WIFI

#define CHIP_APP_MAIN_HAS_ETHERNET_DRIVER 1
DeviceLayer::NetworkCommissioning::DarwinEthernetDriver sEthernetDriver;
#endif // CHIP_DEVICE_LAYER_TARGET_DARWIN

#if CHIP_APP_MAIN_HAS_THREAD_DRIVER
app::Clusters::NetworkCommissioning::Instance sThreadNetworkCommissioningInstance(kRootEndpointId, &sThreadDriver);
#endif // CHIP_APP_MAIN_HAS_THREAD_DRIVER

#if CHIP_APP_MAIN_HAS_WIFI_DRIVER
// The WiFi network commissioning instance cannot be constructed until we know
// whether we have an sSecondaryNetworkCommissioningEndpoint.
Optional<app::Clusters::NetworkCommissioning::Instance> sWiFiNetworkCommissioningInstance;
#endif // CHIP_APP_MAIN_HAS_WIFI_DRIVER

#if CHIP_APP_MAIN_HAS_ETHERNET_DRIVER
app::Clusters::NetworkCommissioning::Instance sEthernetNetworkCommissioningInstance(kRootEndpointId, &sEthernetDriver);
#endif // CHIP_APP_MAIN_HAS_ETHERNET_DRIVER

void EnableThreadNetworkCommissioning()
{
#if CHIP_APP_MAIN_HAS_THREAD_DRIVER
    sThreadNetworkCommissioningInstance.Init();
#endif // CHIP_APP_MAIN_HAS_THREAD_DRIVER
}

void EnableWiFiNetworkCommissioning(EndpointId endpoint)
{
#if CHIP_APP_MAIN_HAS_WIFI_DRIVER
    sWiFiNetworkCommissioningInstance.Emplace(endpoint, &sWiFiDriver);
    sWiFiNetworkCommissioningInstance.Value().Init();
#endif // CHIP_APP_MAIN_HAS_WIFI_DRIVER
}

void InitNetworkCommissioning()
{
    if (sSecondaryNetworkCommissioningEndpoint.HasValue())
    {
        // Enable secondary endpoint only when we need it, this should be applied to all platforms.
        emberAfEndpointEnableDisable(sSecondaryNetworkCommissioningEndpoint.Value(), false);
    }

    const bool kThreadEnabled = {
#if CHIP_APP_MAIN_HAS_THREAD_DRIVER
        LinuxDeviceOptions::GetInstance().mThread
#else
        false
#endif
    };

    const bool kWiFiEnabled = {
#if CHIP_APP_MAIN_HAS_WIFI_DRIVER
        LinuxDeviceOptions::GetInstance().mWiFi
#else
        false
#endif
    };

    if (kThreadEnabled && kWiFiEnabled)
    {
        if (sSecondaryNetworkCommissioningEndpoint.HasValue())
        {
            EnableThreadNetworkCommissioning();
            EnableWiFiNetworkCommissioning(sSecondaryNetworkCommissioningEndpoint.Value());
            // Only enable secondary endpoint for network commissioning cluster when both WiFi and Thread are enabled.
            emberAfEndpointEnableDisable(sSecondaryNetworkCommissioningEndpoint.Value(), true);
        }
        else
        {
            // Just use the Thread one.
            EnableThreadNetworkCommissioning();
        }
    }
    else if (kThreadEnabled)
    {
        EnableThreadNetworkCommissioning();
    }
    else if (kWiFiEnabled)
    {
        EnableWiFiNetworkCommissioning(kRootEndpointId);
    }
    else
    {
#if CHIP_APP_MAIN_HAS_ETHERNET_DRIVER
        sEthernetNetworkCommissioningInstance.Init();
#endif // CHIP_APP_MAIN_HAS_ETHERNET_DRIVER
    }
}
} // anonymous namespace

#if defined(ENABLE_CHIP_SHELL)
using chip::Shell::Engine;
#endif

#if CHIP_DEVICE_CONFIG_ENABLE_WPA
/*
 * The device shall check every kWiFiStartCheckTimeUsec whether Wi-Fi management
 * has been fully initialized. If after kWiFiStartCheckAttempts Wi-Fi management
 * still hasn't been initialized, the device configuration is reset, and device
 * needs to be paired again.
 */
static constexpr useconds_t kWiFiStartCheckTimeUsec = 100 * 1000; // 100 ms
static constexpr uint8_t kWiFiStartCheckAttempts    = 5;
#endif

namespace {
AppMainLoopImplementation * gMainLoopImplementation = nullptr;

// To hold SPAKE2+ verifier, discriminator, passcode
LinuxCommissionableDataProvider gCommissionableDataProvider;

chip::DeviceLayer::DeviceInfoProviderImpl gExampleDeviceInfoProvider;

void EventHandler(const DeviceLayer::ChipDeviceEvent * event, intptr_t arg)
{
    (void) arg;
    if (event->Type == DeviceLayer::DeviceEventType::kCHIPoBLEConnectionEstablished)
    {
        ChipLogProgress(DeviceLayer, "Receive kCHIPoBLEConnectionEstablished");
    }
}

void Cleanup()
{
#if CHIP_CONFIG_TRANSPORT_TRACE_ENABLED
    chip::trace::DeInitTrace();
#endif // CHIP_CONFIG_TRANSPORT_TRACE_ENABLED

    // TODO(16968): Lifecycle management of storage-using components like GroupDataProvider, etc
}

// TODO(#20664) REPL test will fail if signal SIGINT is not caught, temporarily keep following logic.

// when the shell is enabled, don't intercept signals since it prevents the user from
// using expected commands like CTRL-C to quit the application. (see issue #17845)
// We should stop using signals for those faults, and move to a different notification
// means, like a pipe. (see issue #19114)
#if !defined(ENABLE_CHIP_SHELL)
void StopSignalHandler(int signal)
{
    if (gMainLoopImplementation != nullptr)
    {
        gMainLoopImplementation->SignalSafeStopMainLoop();
    }
    else
    {
        Server::GetInstance().GenerateShutDownEvent();
        PlatformMgr().ScheduleWork([](intptr_t) { PlatformMgr().StopEventLoopTask(); });
    }
}
#endif // !defined(ENABLE_CHIP_SHELL)

} // namespace

#if CHIP_DEVICE_CONFIG_ENABLE_WPA
static bool EnsureWiFiIsStarted()
{
    for (int cnt = 0; cnt < kWiFiStartCheckAttempts; cnt++)
    {
        if (DeviceLayer::ConnectivityMgrImpl().IsWiFiManagementStarted())
        {
            return true;
        }

        usleep(kWiFiStartCheckTimeUsec);
    }

    return DeviceLayer::ConnectivityMgrImpl().IsWiFiManagementStarted();
}
#endif

class SampleTestEventTriggerDelegate : public TestEventTriggerDelegate
{
public:
    /// NOTE: If you copy this, please use the reserved range FFFF_FFFF_<VID_HEX>_xxxx for your trigger codes.
    static constexpr uint64_t kSampleTestEventTriggerAlwaysSuccess = static_cast<uint64_t>(0xFFFF'FFFF'FFF1'0000ull);

    SampleTestEventTriggerDelegate() { memset(&mEnableKey[0], 0, sizeof(mEnableKey)); }

    /**
     * @brief Initialize the delegate with a key and an optional other handler
     *
     * The `otherDelegate` will be called if there is no match of the eventTrigger
     * when HandleEventTrigger is called, if it is non-null.
     *
     * @param enableKey - EnableKey to use for this instance.
     * @param otherDelegate - Other delegate (e.g. OTA delegate) where defer trigger. Can be nullptr
     * @return CHIP_NO_ERROR on success, CHIP_ERROR_INVALID_ARGUMENT if enableKey is wrong size.
     */
    CHIP_ERROR Init(ByteSpan enableKey, TestEventTriggerDelegate * otherDelegate)
    {
        VerifyOrReturnError(enableKey.size() == sizeof(mEnableKey), CHIP_ERROR_INVALID_ARGUMENT);
        mOtherDelegate = otherDelegate;
        MutableByteSpan ourEnableKeySpan(mEnableKey);
        return CopySpanToMutableSpan(enableKey, ourEnableKeySpan);
    }

    bool DoesEnableKeyMatch(const ByteSpan & enableKey) const override { return enableKey.data_equal(ByteSpan(mEnableKey)); }

    CHIP_ERROR HandleEventTrigger(uint64_t eventTrigger) override
    {
        ChipLogProgress(Support, "Saw TestEventTrigger: " ChipLogFormatX64, ChipLogValueX64(eventTrigger));

        if (eventTrigger == kSampleTestEventTriggerAlwaysSuccess)
        {
            // Do nothing, successfully
            ChipLogProgress(Support, "Handling \"Always success\" internal test event");
            return CHIP_NO_ERROR;
        }

        return (mOtherDelegate != nullptr) ? mOtherDelegate->HandleEventTrigger(eventTrigger) : CHIP_ERROR_INVALID_ARGUMENT;
    }

private:
    uint8_t mEnableKey[TestEventTriggerDelegate::kEnableKeyLength];
    TestEventTriggerDelegate * mOtherDelegate = nullptr;
};

int ChipLinuxAppInit(int argc, char * const argv[], OptionSet * customOptions,
                     const Optional<EndpointId> secondaryNetworkCommissioningEndpoint)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
#if CONFIG_NETWORK_LAYER_BLE
    RendezvousInformationFlags rendezvousFlags = RendezvousInformationFlag::kBLE;
#else  // CONFIG_NETWORK_LAYER_BLE
    RendezvousInformationFlag rendezvousFlags = RendezvousInformationFlag::kOnNetwork;
#endif // CONFIG_NETWORK_LAYER_BLE

#ifdef CONFIG_RENDEZVOUS_MODE
    rendezvousFlags = static_cast<RendezvousInformationFlags>(CONFIG_RENDEZVOUS_MODE);
#endif

    err = Platform::MemoryInit();
    SuccessOrExit(err);

    err = ParseArguments(argc, argv, customOptions);
    SuccessOrExit(err);

    sSecondaryNetworkCommissioningEndpoint = secondaryNetworkCommissioningEndpoint;

#ifdef CHIP_CONFIG_KVS_PATH
    if (LinuxDeviceOptions::GetInstance().KVS == nullptr)
    {
        err = DeviceLayer::PersistedStorage::KeyValueStoreMgrImpl().Init(CHIP_CONFIG_KVS_PATH);
    }
    else
    {
        err = DeviceLayer::PersistedStorage::KeyValueStoreMgrImpl().Init(LinuxDeviceOptions::GetInstance().KVS);
    }
    SuccessOrExit(err);
#endif

    err = DeviceLayer::PlatformMgr().InitChipStack();
    SuccessOrExit(err);

    // Init the commissionable data provider based on command line options
    // to handle custom verifiers, discriminators, etc.
    err = chip::examples::InitCommissionableDataProvider(gCommissionableDataProvider, LinuxDeviceOptions::GetInstance());
    SuccessOrExit(err);
    DeviceLayer::SetCommissionableDataProvider(&gCommissionableDataProvider);

    err = chip::examples::InitConfigurationManager(reinterpret_cast<ConfigurationManagerImpl &>(ConfigurationMgr()),
                                                   LinuxDeviceOptions::GetInstance());
    SuccessOrExit(err);

    if (LinuxDeviceOptions::GetInstance().payload.rendezvousInformation.HasValue())
    {
        rendezvousFlags = LinuxDeviceOptions::GetInstance().payload.rendezvousInformation.Value();
    }

    err = GetPayloadContents(LinuxDeviceOptions::GetInstance().payload, rendezvousFlags);
    SuccessOrExit(err);

    ConfigurationMgr().LogDeviceConfig();

    {
        ChipLogProgress(NotSpecified, "==== Onboarding payload for Standard Commissioning Flow ====");
        PrintOnboardingCodes(LinuxDeviceOptions::GetInstance().payload);
    }

#if defined(PW_RPC_ENABLED)
    rpc::Init();
    ChipLogProgress(NotSpecified, "PW_RPC initialized.");
#endif // defined(PW_RPC_ENABLED)

    DeviceLayer::PlatformMgrImpl().AddEventHandler(EventHandler, 0);

#if CHIP_CONFIG_TRANSPORT_TRACE_ENABLED
    if (LinuxDeviceOptions::GetInstance().traceStreamFilename.HasValue())
    {
        const char * traceFilename = LinuxDeviceOptions::GetInstance().traceStreamFilename.Value().c_str();
        auto traceStream           = new chip::trace::TraceStreamFile(traceFilename);
        chip::trace::AddTraceStream(traceStream);
    }
    else if (LinuxDeviceOptions::GetInstance().traceStreamToLogEnabled)
    {
        auto traceStream = new chip::trace::TraceStreamLog();
        chip::trace::AddTraceStream(traceStream);
    }

    if (LinuxDeviceOptions::GetInstance().traceStreamDecodeEnabled)
    {
        chip::trace::TraceDecoderOptions options;
        options.mEnableProtocolInteractionModelResponse = false;

        chip::trace::TraceDecoder * decoder = new chip::trace::TraceDecoder();
        decoder->SetOptions(options);
        chip::trace::AddTraceStream(decoder);
    }
    chip::trace::InitTrace();
#endif // CHIP_CONFIG_TRANSPORT_TRACE_ENABLED

#if CONFIG_NETWORK_LAYER_BLE
    DeviceLayer::ConnectivityMgr().SetBLEDeviceName(nullptr); // Use default device name (CHIP-XXXX)
    DeviceLayer::Internal::BLEMgrImpl().ConfigureBle(LinuxDeviceOptions::GetInstance().mBleDevice, false);
    DeviceLayer::ConnectivityMgr().SetBLEAdvertisingEnabled(true);
#endif

#if CHIP_DEVICE_CONFIG_ENABLE_WPA
    if (LinuxDeviceOptions::GetInstance().mWiFi)
    {
        DeviceLayer::ConnectivityMgrImpl().StartWiFiManagement();
        if (!EnsureWiFiIsStarted())
        {
            ChipLogError(NotSpecified, "Wi-Fi Management taking too long to start - device configuration will be reset.");
        }
    }
#endif // CHIP_DEVICE_CONFIG_ENABLE_WPA

#if CHIP_ENABLE_OPENTHREAD
    if (LinuxDeviceOptions::GetInstance().mThread)
    {
        SuccessOrExit(err = DeviceLayer::ThreadStackMgrImpl().InitThreadStack());
        ChipLogProgress(NotSpecified, "Thread initialized.");
    }
#endif // CHIP_ENABLE_OPENTHREAD

exit:
    if (err != CHIP_NO_ERROR)
    {
        ChipLogProgress(NotSpecified, "Failed to init Linux App: %s ", ErrorStr(err));
        Cleanup();

        // End the program with non zero error code to indicate a error.
        return 1;
    }
    return 0;
}

void ChipLinuxAppMainLoop(AppMainLoopImplementation * impl)
{
    gMainLoopImplementation = impl;

    static chip::CommonCaseDeviceServerInitParams initParams;
    VerifyOrDie(initParams.InitializeStaticResourcesBeforeServerInit() == CHIP_NO_ERROR);

#if defined(ENABLE_CHIP_SHELL)
    Engine::Root().Init();
    std::thread shellThread([]() { Engine::Root().RunMainLoop(); });
    Shell::RegisterCommissioneeCommands();
#endif
    initParams.operationalServicePort        = CHIP_PORT;
    initParams.userDirectedCommissioningPort = CHIP_UDC_PORT;

#if CHIP_DEVICE_CONFIG_ENABLE_BOTH_COMMISSIONER_AND_COMMISSIONEE || CHIP_DEVICE_ENABLE_PORT_PARAMS
    // use a different service port to make testing possible with other sample devices running on same host
    initParams.operationalServicePort        = LinuxDeviceOptions::GetInstance().securedDevicePort;
    initParams.userDirectedCommissioningPort = LinuxDeviceOptions::GetInstance().unsecuredCommissionerPort;
#endif // CHIP_DEVICE_CONFIG_ENABLE_BOTH_COMMISSIONER_AND_COMMISSIONEE

#if ENABLE_TRACING
    chip::CommandLineApp::TracingSetup tracing_setup;

    for (const auto & trace_destination : LinuxDeviceOptions::GetInstance().traceTo)
    {
        tracing_setup.EnableTracingFor(trace_destination.c_str());
    }
#endif

    initParams.interfaceId = LinuxDeviceOptions::GetInstance().interfaceId;

    if (LinuxDeviceOptions::GetInstance().mCSRResponseOptions.csrExistingKeyPair)
    {
        LinuxDeviceOptions::GetInstance().mCSRResponseOptions.badCsrOperationalKeyStoreForTest.Init(
            initParams.persistentStorageDelegate);
        initParams.operationalKeystore = &LinuxDeviceOptions::GetInstance().mCSRResponseOptions.badCsrOperationalKeyStoreForTest;
    }

    TestEventTriggerDelegate * otherDelegate = nullptr;
#if CHIP_DEVICE_CONFIG_ENABLE_OTA_REQUESTOR
    // We want to allow triggering OTA queries if OTA requestor is enabled
    static OTATestEventTriggerDelegate otaTestEventTriggerDelegate{ ByteSpan(
        LinuxDeviceOptions::GetInstance().testEventTriggerEnableKey) };
    otherDelegate = &otaTestEventTriggerDelegate;
#endif
#if CHIP_DEVICE_CONFIG_ENABLE_SMOKE_CO_TRIGGER
    static SmokeCOTestEventTriggerDelegate smokeCOTestEventTriggerDelegate{
        ByteSpan(LinuxDeviceOptions::GetInstance().testEventTriggerEnableKey), otherDelegate
    };
    otherDelegate = &smokeCOTestEventTriggerDelegate;
#endif
    // For general testing of TestEventTrigger, we have a common "core" event trigger delegate.
    static SampleTestEventTriggerDelegate testEventTriggerDelegate;
    VerifyOrDie(testEventTriggerDelegate.Init(ByteSpan(LinuxDeviceOptions::GetInstance().testEventTriggerEnableKey),
                                              otherDelegate) == CHIP_NO_ERROR);

    initParams.testEventTriggerDelegate = &testEventTriggerDelegate;

    // We need to set DeviceInfoProvider before Server::Init to setup the storage of DeviceInfoProvider properly.
    DeviceLayer::SetDeviceInfoProvider(&gExampleDeviceInfoProvider);

    chip::app::RuntimeOptionsProvider::Instance().SetSimulateNoInternalTime(
        LinuxDeviceOptions::GetInstance().mSimulateNoInternalTime);

    // Init ZCL Data Model and CHIP App Server
    Server::GetInstance().Init(initParams);

    // Now that the server has started and we are done with our startup logging,
    // log our discovery/onboarding information again so it's not lost in the
    // noise.
    ConfigurationMgr().LogDeviceConfig();

    PrintOnboardingCodes(LinuxDeviceOptions::GetInstance().payload);

    // Initialize device attestation config
    SetDeviceAttestationCredentialsProvider(LinuxDeviceOptions::GetInstance().dacProvider);

#if CHIP_DEVICE_CONFIG_ENABLE_BOTH_COMMISSIONER_AND_COMMISSIONEE
    ChipLogProgress(AppServer, "Starting commissioner");
    VerifyOrReturn(InitCommissioner(LinuxDeviceOptions::GetInstance().securedCommissionerPort,
                                    LinuxDeviceOptions::GetInstance().unsecuredCommissionerPort,
                                    LinuxDeviceOptions::GetInstance().commissionerFabricId) == CHIP_NO_ERROR);
    ChipLogProgress(AppServer, "Started commissioner");
#if defined(ENABLE_CHIP_SHELL)
    Shell::RegisterControllerCommands();
#endif // defined(ENABLE_CHIP_SHELL)
#endif // CHIP_DEVICE_CONFIG_ENABLE_BOTH_COMMISSIONER_AND_COMMISSIONEE

    InitNetworkCommissioning();

    ApplicationInit();

#if !defined(ENABLE_CHIP_SHELL)
    // NOLINTBEGIN(bugprone-signal-handler)
    signal(SIGINT, StopSignalHandler);
    signal(SIGTERM, StopSignalHandler);
    // NOLINTEND(bugprone-signal-handler)
#endif // !defined(ENABLE_CHIP_SHELL)

    if (impl != nullptr)
    {
        impl->RunMainLoop();
    }
    else
    {
        DeviceLayer::PlatformMgr().RunEventLoop();
    }
    gMainLoopImplementation = nullptr;

    ApplicationShutdown();

#if CHIP_DEVICE_CONFIG_ENABLE_BOTH_COMMISSIONER_AND_COMMISSIONEE
    ShutdownCommissioner();
#endif // CHIP_DEVICE_CONFIG_ENABLE_BOTH_COMMISSIONER_AND_COMMISSIONEE

#if defined(ENABLE_CHIP_SHELL)
    shellThread.join();
#endif

    Server::GetInstance().Shutdown();

#if ENABLE_TRACING
    tracing_setup.StopTracing();
#endif

    DeviceLayer::PlatformMgr().Shutdown();

    Cleanup();
}
