// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// This file implements a standalone host process for Me2Me.

#include <string>

#include "base/at_exit.h"
#include "base/bind.h"
#include "base/callback.h"
#include "base/command_line.h"
#include "base/file_path.h"
#include "base/file_util.h"
#include "base/memory/scoped_ptr.h"
#include "base/message_loop.h"
#include "base/scoped_native_library.h"
#include "base/string_number_conversions.h"
#include "base/string_util.h"
#include "base/stringize_macros.h"
#include "base/synchronization/waitable_event.h"
#include "base/threading/thread.h"
#include "base/utf_string_conversions.h"
#include "base/win/windows_version.h"
#include "build/build_config.h"
#include "crypto/nss_util.h"
#include "ipc/ipc_channel.h"
#include "ipc/ipc_channel_proxy.h"
#include "ipc/ipc_listener.h"
#include "net/base/network_change_notifier.h"
#include "net/socket/ssl_server_socket.h"
#include "remoting/base/auto_thread_task_runner.h"
#include "remoting/base/breakpad.h"
#include "remoting/base/constants.h"
#include "remoting/host/branding.h"
#include "remoting/host/chromoting_host.h"
#include "remoting/host/chromoting_host_context.h"
#include "remoting/host/chromoting_messages.h"
#include "remoting/host/config_file_watcher.h"
#include "remoting/host/curtain_mode.h"
#include "remoting/host/curtaining_host_observer.h"
#include "remoting/host/desktop_environment_factory.h"
#include "remoting/host/desktop_resizer.h"
#include "remoting/host/desktop_session_connector.h"
#include "remoting/host/dns_blackhole_checker.h"
#include "remoting/host/event_executor.h"
#include "remoting/host/heartbeat_sender.h"
#include "remoting/host/host_config.h"
#include "remoting/host/host_event_logger.h"
#include "remoting/host/host_exit_codes.h"
#include "remoting/host/host_user_interface.h"
#include "remoting/host/ipc_consts.h"
#include "remoting/host/ipc_desktop_environment_factory.h"
#include "remoting/host/json_host_config.h"
#include "remoting/host/logging.h"
#include "remoting/host/log_to_server.h"
#include "remoting/host/network_settings.h"
#include "remoting/host/policy_hack/policy_watcher.h"
#include "remoting/host/resizing_host_observer.h"
#include "remoting/host/session_manager_factory.h"
#include "remoting/host/signaling_connector.h"
#include "remoting/host/usage_stats_consent.h"
#include "remoting/host/video_frame_capturer.h"
#include "remoting/jingle_glue/xmpp_signal_strategy.h"
#include "remoting/protocol/me2me_host_authenticator_factory.h"

#if defined(OS_POSIX)
#include <signal.h>
#include "base/file_descriptor_posix.h"
#include "remoting/host/posix/signal_handler.h"
#endif  // defined(OS_POSIX)

#if defined(OS_MACOSX)
#include "base/mac/scoped_cftyperef.h"
#include "base/mac/scoped_nsautorelease_pool.h"
#endif  // defined(OS_MACOSX)

#if defined(OS_LINUX)
#include "remoting/host/audio_capturer_linux.h"
#include "remoting/host/pam_authorization_factory_posix.h"
#endif  // defined(OS_LINUX)

// N.B. OS_WIN is defined by including src/base headers.
#if defined(OS_WIN)
#include <commctrl.h>
#include "base/win/scoped_handle.h"
#include "remoting/host/win/session_desktop_environment_factory.h"
#endif  // defined(OS_WIN)

#if defined(TOOLKIT_GTK)
#include "ui/gfx/gtk_util.h"
#endif  // defined(TOOLKIT_GTK)

namespace {

// This is used for tagging system event logs.
const char kApplicationName[] = "chromoting";

// The command line switch used to get version of the daemon.
const char kVersionSwitchName[] = "version";

// The command line switch used to pass name of the pipe to capture audio on
// linux.
const char kAudioPipeSwitchName[] = "audio-pipe-name";

void QuitMessageLoop(MessageLoop* message_loop) {
  message_loop->PostTask(FROM_HERE, MessageLoop::QuitClosure());
}

}  // namespace

namespace remoting {

class HostProcess
    : public ConfigFileWatcher::Delegate,
      public HeartbeatSender::Listener,
      public IPC::Listener {
 public:
  explicit HostProcess(scoped_ptr<ChromotingHostContext> context);

  // Initializes IPC control channel and config file path from |cmd_line|.
  bool InitWithCommandLine(const CommandLine* cmd_line);

  // ConfigFileWatcher::Delegate interface.
  virtual void OnConfigUpdated(const std::string& serialized_config) OVERRIDE;
  virtual void OnConfigWatcherError() OVERRIDE;

  void StartWatchingConfigChanges();
  void CreateAuthenticatorFactory();

  // IPC::Listener implementation.
  virtual bool OnMessageReceived(const IPC::Message& message) OVERRIDE;
  virtual void OnChannelError() OVERRIDE;

  // HeartbeatSender::Listener overrides.
  virtual void OnUnknownHostIdError() OVERRIDE;

  void StartHostProcess();

  int get_exit_code() const;

 private:
#if defined(OS_POSIX)
  // Registers a SIGTERM handler on the network thread, to shutdown the host.
  void ListenForShutdownSignal();

  // Callback passed to RegisterSignalHandler() to handle SIGTERM events.
  void SigTermHandler(int signal_number);
#endif

  // Asks the daemon to inject Secure Attention Sequence to the console.
  void SendSasToConsole();

  void ShutdownHostProcess();

  // Applies the host config, returning true if successful.
  bool ApplyConfig(scoped_ptr<JsonHostConfig> config);

  void OnPolicyUpdate(scoped_ptr<base::DictionaryValue> policies);
  bool OnHostDomainPolicyUpdate(const std::string& host_domain);
  bool OnNatPolicyUpdate(bool nat_traversal_enabled);
  bool OnCurtainPolicyUpdate(bool curtain_required);
  bool OnHostTalkGadgetPrefixPolicyUpdate(const std::string& talkgadget_prefix);

  void StartHost();

  void OnAuthFailed();

  void RejectAuthenticatingClient();

  // Invoked when the user uses the Disconnect windows to terminate
  // the sessions, or when the local session is activated in curtain mode.
  void OnDisconnectRequested();

  void RestartHost();

  void RestartOnHostShutdown();

  void Shutdown(int exit_code);

  void OnShutdownFinished();

  void ResetHost();

  // Crashes the process in response to a daemon's request. The daemon passes
  // the location of the code that detected the fatal error resulted in this
  // request.
  void OnCrash(const std::string& function_name,
               const std::string& file_name,
               const int& line_number);

  scoped_ptr<ChromotingHostContext> context_;
  scoped_ptr<IPC::ChannelProxy> daemon_channel_;
  scoped_ptr<net::NetworkChangeNotifier> network_change_notifier_;

  FilePath host_config_path_;
  scoped_ptr<ConfigFileWatcher> config_watcher_;

  // Accessed on the network thread.
  std::string host_id_;
  protocol::SharedSecretHash host_secret_hash_;
  HostKeyPair key_pair_;
  std::string oauth_refresh_token_;
  std::string serialized_config_;
  std::string xmpp_login_;
  std::string xmpp_auth_token_;
  std::string xmpp_auth_service_;

  scoped_ptr<policy_hack::PolicyWatcher> policy_watcher_;
  bool allow_nat_traversal_;
  std::string talkgadget_prefix_;

  scoped_ptr<CurtainMode> curtain_;
  scoped_ptr<CurtainingHostObserver> curtaining_host_observer_;

  bool restarting_;
  bool shutting_down_;

  scoped_ptr<DesktopEnvironmentFactory> desktop_environment_factory_;
  scoped_ptr<DesktopResizer> desktop_resizer_;
  scoped_ptr<ResizingHostObserver> resizing_host_observer_;
  scoped_ptr<XmppSignalStrategy> signal_strategy_;
  scoped_ptr<SignalingConnector> signaling_connector_;
  scoped_ptr<HeartbeatSender> heartbeat_sender_;
  scoped_ptr<LogToServer> log_to_server_;
  scoped_ptr<HostEventLogger> host_event_logger_;

  scoped_ptr<HostUserInterface> host_user_interface_;

  scoped_refptr<ChromotingHost> host_;

#if defined(REMOTING_MULTI_PROCESS)
  DesktopSessionConnector* desktop_session_connector_;
#endif  // defined(REMOTING_MULTI_PROCESS)

  int exit_code_;
};

HostProcess::HostProcess(scoped_ptr<ChromotingHostContext> context)
    : context_(context.Pass()),
      allow_nat_traversal_(true),
      restarting_(false),
      shutting_down_(false),
      desktop_resizer_(DesktopResizer::Create()),
#if defined(REMOTING_MULTI_PROCESS)
      desktop_session_connector_(NULL),
#endif  // defined(REMOTING_MULTI_PROCESS)
      exit_code_(kSuccessExitCode) {
  network_change_notifier_.reset(net::NetworkChangeNotifier::Create());
  curtain_ = CurtainMode::Create(
      base::Bind(&HostProcess::OnDisconnectRequested,
                 base::Unretained(this)),
      base::Bind(&HostProcess::RejectAuthenticatingClient,
                 base::Unretained(this)));
}

bool HostProcess::InitWithCommandLine(const CommandLine* cmd_line) {
#if defined(REMOTING_MULTI_PROCESS)
  // Parse the handle value and convert it to a handle/file descriptor.
  std::string channel_name =
      cmd_line->GetSwitchValueASCII(kDaemonPipeSwitchName);

  int pipe_handle = 0;
  if (channel_name.empty() ||
      !base::StringToInt(channel_name, &pipe_handle)) {
    LOG(ERROR) << "Invalid '" << kDaemonPipeSwitchName
               << "' value: " << channel_name;
    return false;
  }

#if defined(OS_WIN)
  base::win::ScopedHandle pipe(reinterpret_cast<HANDLE>(pipe_handle));
  IPC::ChannelHandle channel_handle(pipe);
#elif defined(OS_POSIX)
  base::FileDescriptor pipe(pipe_handle, true);
  IPC::ChannelHandle channel_handle(channel_name, pipe);
#endif  // defined(OS_POSIX)

  // Connect to the daemon process.
  daemon_channel_.reset(new IPC::ChannelProxy(
      channel_handle,
      IPC::Channel::MODE_CLIENT,
      this,
      context_->network_task_runner()));
#else  // !defined(REMOTING_MULTI_PROCESS)
  // Connect to the daemon process.
  std::string channel_name =
      cmd_line->GetSwitchValueASCII(kDaemonPipeSwitchName);
  if (!channel_name.empty()) {
    daemon_channel_.reset(new IPC::ChannelProxy(
        channel_name, IPC::Channel::MODE_CLIENT, this,
        context_->network_task_runner()));
  }

  FilePath default_config_dir = remoting::GetConfigDir();
  host_config_path_ = default_config_dir.Append(kDefaultHostConfigFile);
  if (cmd_line->HasSwitch(kHostConfigSwitchName)) {
    host_config_path_ = cmd_line->GetSwitchValuePath(kHostConfigSwitchName);
  }
#endif  // !defined(REMOTING_MULTI_PROCESS)

  return true;
}

void HostProcess::OnConfigUpdated(
    const std::string& serialized_config) {
  if (!context_->network_task_runner()->BelongsToCurrentThread()) {
    context_->network_task_runner()->PostTask(FROM_HERE, base::Bind(
        &HostProcess::OnConfigUpdated, base::Unretained(this),
        serialized_config));
    return;
  }

  // Filter out duplicates.
  if (serialized_config_ == serialized_config)
    return;

  LOG(INFO) << "Processing new host configuration.";

  serialized_config_ = serialized_config;
  scoped_ptr<JsonHostConfig> config(new JsonHostConfig(FilePath()));
  if (!config->SetSerializedData(serialized_config)) {
    LOG(ERROR) << "Invalid configuration.";
    Shutdown(kInvalidHostConfigurationExitCode);
    return;
  }

  if (!ApplyConfig(config.Pass())) {
    LOG(ERROR) << "Failed to apply the configuration.";
    Shutdown(kInvalidHostConfigurationExitCode);
    return;
  }

  // Start watching the policy (and eventually start the host) if this is
  // the first configuration update. Otherwise, create new authenticator
  // factory in case PIN has changed.
  if (!policy_watcher_) {
    policy_watcher_.reset(
        policy_hack::PolicyWatcher::Create(context_->file_task_runner()));
    policy_watcher_->StartWatching(
        base::Bind(&HostProcess::OnPolicyUpdate, base::Unretained(this)));
  } else {
    CreateAuthenticatorFactory();
  }
}

void HostProcess::OnConfigWatcherError() {
  DCHECK(context_->ui_task_runner()->BelongsToCurrentThread());

  context_->network_task_runner()->PostTask(
      FROM_HERE,
      base::Bind(&HostProcess::Shutdown, base::Unretained(this),
                 kInvalidHostConfigurationExitCode));
}

void HostProcess::StartWatchingConfigChanges() {
#if !defined(REMOTING_MULTI_PROCESS)
    // Start watching the host configuration file.
    config_watcher_.reset(new ConfigFileWatcher(context_->ui_task_runner(),
                                                context_->file_task_runner(),
                                                this));
    config_watcher_->Watch(host_config_path_);
#endif  // !defined(REMOTING_MULTI_PROCESS)
}

#if defined(OS_POSIX)
void HostProcess::ListenForShutdownSignal() {
  DCHECK(context_->network_task_runner()->BelongsToCurrentThread());

  remoting::RegisterSignalHandler(
      SIGTERM,
      base::Bind(&HostProcess::SigTermHandler, base::Unretained(this)));
}

void HostProcess::SigTermHandler(int signal_number) {
  DCHECK(signal_number == SIGTERM);
  DCHECK(context_->network_task_runner()->BelongsToCurrentThread());
  LOG(INFO) << "Caught SIGTERM: Shutting down...";
  Shutdown(kSuccessExitCode);
}
#endif  // OS_POSIX

void HostProcess::CreateAuthenticatorFactory() {
  DCHECK(context_->network_task_runner()->BelongsToCurrentThread());

  if (!host_ || shutting_down_)
    return;

  std::string local_certificate = key_pair_.GenerateCertificate();
  if (local_certificate.empty()) {
    LOG(ERROR) << "Failed to generate host certificate.";
    Shutdown(kInitializationFailed);
    return;
  }

  scoped_ptr<protocol::AuthenticatorFactory> factory(
      new protocol::Me2MeHostAuthenticatorFactory(
          local_certificate, *key_pair_.private_key(), host_secret_hash_));
#if defined(OS_LINUX)
  // On Linux, perform a PAM authorization step after authentication.
  factory.reset(new PamAuthorizationFactory(factory.Pass()));
#endif
  host_->SetAuthenticatorFactory(factory.Pass());
}

// IPC::Listener implementation.
bool HostProcess::OnMessageReceived(const IPC::Message& message) {
  DCHECK(context_->ui_task_runner()->BelongsToCurrentThread());

#if defined(REMOTING_MULTI_PROCESS)
  bool handled = true;
  IPC_BEGIN_MESSAGE_MAP(HostProcess, message)
    IPC_MESSAGE_HANDLER(ChromotingDaemonNetworkMsg_Crash,
                        OnCrash)
    IPC_MESSAGE_HANDLER(ChromotingDaemonNetworkMsg_Configuration,
                        OnConfigUpdated)
    IPC_MESSAGE_FORWARD(
        ChromotingDaemonNetworkMsg_DesktopAttached,
        desktop_session_connector_,
        DesktopSessionConnector::OnDesktopSessionAgentAttached)
    IPC_MESSAGE_FORWARD(ChromotingDaemonNetworkMsg_TerminalDisconnected,
                        desktop_session_connector_,
                        DesktopSessionConnector::OnTerminalDisconnected)
    IPC_MESSAGE_UNHANDLED(handled = false)
  IPC_END_MESSAGE_MAP()
  return handled;
#else  // !defined(REMOTING_MULTI_PROCESS)
  return false;
#endif  // !defined(REMOTING_MULTI_PROCESS)
}

void HostProcess::OnChannelError() {
  DCHECK(context_->ui_task_runner()->BelongsToCurrentThread());

  // Shutdown the host if the daemon disconnected the channel.
  context_->network_task_runner()->PostTask(
      FROM_HERE,
      base::Bind(&HostProcess::Shutdown, base::Unretained(this),
                 kSuccessExitCode));
}

void HostProcess::StartHostProcess() {
  DCHECK(context_->ui_task_runner()->BelongsToCurrentThread());

  if (!InitWithCommandLine(CommandLine::ForCurrentProcess())) {
    OnConfigWatcherError();
    return;
  }

  // Create a desktop environment factory appropriate to the build type &
  // platform.
#if defined(OS_WIN)

#if defined(REMOTING_MULTI_PROCESS)
  IpcDesktopEnvironmentFactory* desktop_environment_factory =
      new IpcDesktopEnvironmentFactory(
          daemon_channel_.get(),
          context_->input_task_runner(),
          context_->network_task_runner(),
          context_->ui_task_runner());
  desktop_session_connector_ = desktop_environment_factory;
#else // !defined(REMOTING_MULTI_PROCESS)
  DesktopEnvironmentFactory* desktop_environment_factory =
      new SessionDesktopEnvironmentFactory(
          context_->input_task_runner(), context_->ui_task_runner(),
          base::Bind(&HostProcess::SendSasToConsole, base::Unretained(this)));
#endif  // !defined(REMOTING_MULTI_PROCESS)

#else  // !defined(OS_WIN)
  DesktopEnvironmentFactory* desktop_environment_factory =
      new DesktopEnvironmentFactory(
          context_->input_task_runner(), context_->ui_task_runner());
#endif  // !defined(OS_WIN)

  desktop_environment_factory_.reset(desktop_environment_factory);

#if defined(OS_POSIX)
  context_->network_task_runner()->PostTask(
      FROM_HERE,
      base::Bind(&HostProcess::ListenForShutdownSignal,
                 base::Unretained(this)));
#endif // OS_POSIX

  // The host UI should be created on the UI thread.
  bool want_user_interface = true;
#if defined(OS_LINUX)
  want_user_interface = false;
#elif defined(OS_MACOSX)
  // Don't try to display any UI on top of the system's login screen as this
  // is rejected by the Window Server on OS X 10.7.4, and prevents the
  // capturer from working (http://crbug.com/140984).

  // TODO(lambroslambrou): Use a better technique of detecting whether we're
  // running in the LoginWindow context, and refactor this into a separate
  // function to be used here and in CurtainMode::ActivateCurtain().
  want_user_interface = getuid() != 0;
#endif  // OS_MACOSX

  if (want_user_interface) {
    host_user_interface_.reset(
        new HostUserInterface(context_->network_task_runner(),
                              context_->ui_task_runner()));
    host_user_interface_->Init();
  }

  StartWatchingConfigChanges();
}

int HostProcess::get_exit_code() const {
  return exit_code_;
}

void HostProcess::SendSasToConsole() {
  DCHECK(context_->ui_task_runner()->BelongsToCurrentThread());

  if (daemon_channel_)
    daemon_channel_->Send(new ChromotingNetworkDaemonMsg_SendSasToConsole());
}

void HostProcess::ShutdownHostProcess() {
  DCHECK(context_->ui_task_runner()->BelongsToCurrentThread());

  // Tear down resources that use ChromotingHostContext threads.
  config_watcher_.reset();
  daemon_channel_.reset();
  desktop_environment_factory_.reset();
  host_user_interface_.reset();

  context_.reset();
}

// Overridden from HeartbeatSender::Listener
void HostProcess::OnUnknownHostIdError() {
  LOG(ERROR) << "Host ID not found.";
  Shutdown(kInvalidHostIdExitCode);
}

// Applies the host config, returning true if successful.
bool HostProcess::ApplyConfig(scoped_ptr<JsonHostConfig> config) {
  DCHECK(context_->network_task_runner()->BelongsToCurrentThread());

  if (!config->GetString(kHostIdConfigPath, &host_id_)) {
    LOG(ERROR) << "host_id is not defined in the config.";
    return false;
  }

  if (!key_pair_.Load(*config)) {
    return false;
  }

  std::string host_secret_hash_string;
  if (!config->GetString(kHostSecretHashConfigPath,
                         &host_secret_hash_string)) {
    host_secret_hash_string = "plain:";
  }

  if (!host_secret_hash_.Parse(host_secret_hash_string)) {
    LOG(ERROR) << "Invalid host_secret_hash.";
    return false;
  }

  // Use an XMPP connection to the Talk network for session signalling.
  if (!config->GetString(kXmppLoginConfigPath, &xmpp_login_) ||
      !(config->GetString(kXmppAuthTokenConfigPath, &xmpp_auth_token_) ||
        config->GetString(kOAuthRefreshTokenConfigPath,
                          &oauth_refresh_token_))) {
    LOG(ERROR) << "XMPP credentials are not defined in the config.";
    return false;
  }

  if (!oauth_refresh_token_.empty()) {
    xmpp_auth_token_ = "";  // This will be set to the access token later.
    xmpp_auth_service_ = "oauth2";
  } else if (!config->GetString(kXmppAuthServiceConfigPath,
                                &xmpp_auth_service_)) {
    // For the me2me host, we default to ClientLogin token for chromiumsync
    // because earlier versions of the host had no HTTP stack with which to
    // request an OAuth2 access token.
    xmpp_auth_service_ = kChromotingTokenDefaultServiceName;
  }
  return true;
}

void HostProcess::OnPolicyUpdate(scoped_ptr<base::DictionaryValue> policies) {
  // TODO(rmsousa): Consolidate all On*PolicyUpdate methods into this one.
  if (!context_->network_task_runner()->BelongsToCurrentThread()) {
    context_->network_task_runner()->PostTask(FROM_HERE, base::Bind(
        &HostProcess::OnPolicyUpdate, base::Unretained(this),
        base::Passed(&policies)));
    return;
  }

  bool restart_required = false;
  bool bool_value;
  std::string string_value;
  if (policies->GetString(policy_hack::PolicyWatcher::kHostDomainPolicyName,
                          &string_value)) {
    restart_required |= OnHostDomainPolicyUpdate(string_value);
  }
  if (policies->GetBoolean(policy_hack::PolicyWatcher::kNatPolicyName,
                           &bool_value)) {
    restart_required |= OnNatPolicyUpdate(bool_value);
  }
  if (policies->GetString(
          policy_hack::PolicyWatcher::kHostTalkGadgetPrefixPolicyName,
          &string_value)) {
    restart_required |= OnHostTalkGadgetPrefixPolicyUpdate(string_value);
  }
  if (policies->GetBoolean(
          policy_hack::PolicyWatcher::kHostRequireCurtainPolicyName,
          &bool_value)) {
      restart_required |= OnCurtainPolicyUpdate(bool_value);
  }
  if (!host_) {
    StartHost();
  } else if (restart_required) {
    RestartHost();
  }
}

bool HostProcess::OnHostDomainPolicyUpdate(const std::string& host_domain) {
  // Returns true if the host has to be restarted after this policy update.
  DCHECK(context_->network_task_runner()->BelongsToCurrentThread());

  if (!host_domain.empty() &&
      !EndsWith(xmpp_login_, std::string("@") + host_domain, false)) {
    Shutdown(kInvalidHostDomainExitCode);
  }
  return false;
}

bool HostProcess::OnNatPolicyUpdate(bool nat_traversal_enabled) {
  // Returns true if the host has to be restarted after this policy update.
  DCHECK(context_->network_task_runner()->BelongsToCurrentThread());

  if (allow_nat_traversal_ != nat_traversal_enabled) {
    allow_nat_traversal_ = nat_traversal_enabled;
    LOG(INFO) << "Updated NAT policy.";
    return true;
  }
  return false;
}

bool HostProcess::OnCurtainPolicyUpdate(bool curtain_required) {
  // Returns true if the host has to be restarted after this policy update.
  DCHECK(context_->network_task_runner()->BelongsToCurrentThread());

#if defined(OS_MACOSX)
  if (curtain_required) {
    // If curtain mode is required, then we can't currently support remoting
    // the login screen. This is because we don't curtain the login screen
    // and the current daemon architecture means that the connction is closed
    // immediately after login, leaving the host system uncurtained.
    //
    // TODO(jamiewalch): Fix this once we have implemented the multi-process
    // daemon architecture (crbug.com/134894)
    if (getuid() == 0) {
      Shutdown(kLoginScreenNotSupportedExitCode);
      return false;
    }
  }
#endif
  if (curtain_->required() != curtain_required) {
    LOG(INFO) << "Updated curtain policy.";
    curtain_->set_required(curtain_required);
    return true;
  }
  return false;
}

bool HostProcess::OnHostTalkGadgetPrefixPolicyUpdate(
    const std::string& talkgadget_prefix) {
  // Returns true if the host has to be restarted after this policy update.
  DCHECK(context_->network_task_runner()->BelongsToCurrentThread());

  if (talkgadget_prefix != talkgadget_prefix_) {
    LOG(INFO) << "Updated talkgadget policy.";
    talkgadget_prefix_ = talkgadget_prefix;
    return true;
  }
  return false;
}

void HostProcess::StartHost() {
  DCHECK(context_->network_task_runner()->BelongsToCurrentThread());
  DCHECK(!host_);
  DCHECK(!signal_strategy_.get());

  if (shutting_down_)
    return;

  signal_strategy_.reset(
      new XmppSignalStrategy(context_->url_request_context_getter(),
                             xmpp_login_, xmpp_auth_token_,
                             xmpp_auth_service_));

  scoped_ptr<DnsBlackholeChecker> dns_blackhole_checker(
      new DnsBlackholeChecker(context_->url_request_context_getter(),
                              talkgadget_prefix_));

  signaling_connector_.reset(new SignalingConnector(
      signal_strategy_.get(),
      context_->url_request_context_getter(),
      dns_blackhole_checker.Pass(),
      base::Bind(&HostProcess::OnAuthFailed, base::Unretained(this))));

  if (!oauth_refresh_token_.empty()) {
    scoped_ptr<SignalingConnector::OAuthCredentials> oauth_credentials(
        new SignalingConnector::OAuthCredentials(
            xmpp_login_, oauth_refresh_token_));
    signaling_connector_->EnableOAuth(oauth_credentials.Pass());
  }

  NetworkSettings network_settings(
      allow_nat_traversal_ ?
      NetworkSettings::NAT_TRAVERSAL_ENABLED :
      NetworkSettings::NAT_TRAVERSAL_DISABLED);
  if (!allow_nat_traversal_) {
    network_settings.min_port = NetworkSettings::kDefaultMinPort;
    network_settings.max_port = NetworkSettings::kDefaultMaxPort;
  }

  host_ = new ChromotingHost(
      signal_strategy_.get(),
      desktop_environment_factory_.get(),
      CreateHostSessionManager(network_settings,
                               context_->url_request_context_getter()),
      context_->capture_task_runner(),
      context_->encode_task_runner(),
      context_->network_task_runner());

  // TODO(simonmorris): Get the maximum session duration from a policy.
#if defined(OS_LINUX)
  host_->SetMaximumSessionDuration(base::TimeDelta::FromHours(20));
#endif

  heartbeat_sender_.reset(new HeartbeatSender(
      this, host_id_, signal_strategy_.get(), &key_pair_));

  log_to_server_.reset(
      new LogToServer(host_, ServerLogEntry::ME2ME, signal_strategy_.get()));
  host_event_logger_ = HostEventLogger::Create(host_, kApplicationName);

#if defined(OS_LINUX)
  // Desktop resizing is implemented on all three platforms, but may not be
  // the right thing to do for non-virtual desktops. Disable it until we can
  // implement a configuration UI.
  resizing_host_observer_.reset(
      new ResizingHostObserver(desktop_resizer_.get(), host_));
#endif

  // Curtain mode is currently broken on Mac (the only supported platform),
  // so it's disabled until we've had time to fully investigate.
  //    curtaining_host_observer_.reset(new CurtainingHostObserver(
  //          curtain_.get(), host_));

  if (host_user_interface_.get()) {
    host_user_interface_->Start(
        host_, base::Bind(&HostProcess::OnDisconnectRequested,
                          base::Unretained(this)));
  }

  host_->Start(xmpp_login_);

  CreateAuthenticatorFactory();
}

void HostProcess::OnAuthFailed() {
  Shutdown(kInvalidOauthCredentialsExitCode);
}

void HostProcess::RejectAuthenticatingClient() {
  DCHECK(context_->network_task_runner()->BelongsToCurrentThread());
  DCHECK(host_);
  host_->RejectAuthenticatingClient();
}

// Invoked when the user uses the Disconnect windows to terminate
// the sessions, or when the local session is activated in curtain mode.
void HostProcess::OnDisconnectRequested() {
  if (!context_->network_task_runner()->BelongsToCurrentThread()) {
    context_->network_task_runner()->PostTask(FROM_HERE, base::Bind(
        &HostProcess::OnDisconnectRequested, base::Unretained(this)));
    return;
  }
  if (host_) {
    host_->DisconnectAllClients();
  }
}

void HostProcess::RestartHost() {
  DCHECK(context_->network_task_runner()->BelongsToCurrentThread());

  if (restarting_ || shutting_down_)
    return;

  restarting_ = true;
  host_->Shutdown(base::Bind(
      &HostProcess::RestartOnHostShutdown, base::Unretained(this)));
}

void HostProcess::RestartOnHostShutdown() {
  DCHECK(context_->network_task_runner()->BelongsToCurrentThread());

  if (shutting_down_)
    return;

  restarting_ = false;
  host_ = NULL;
  ResetHost();

  StartHost();
}

void HostProcess::Shutdown(int exit_code) {
  DCHECK(context_->network_task_runner()->BelongsToCurrentThread());

  if (shutting_down_)
    return;

  shutting_down_ = true;
  exit_code_ = exit_code;
  if (host_) {
    host_->Shutdown(base::Bind(
        &HostProcess::OnShutdownFinished, base::Unretained(this)));
  } else {
    OnShutdownFinished();
  }
}

void HostProcess::OnShutdownFinished() {
  DCHECK(context_->network_task_runner()->BelongsToCurrentThread());

  // Destroy networking objects while we are on the network thread.
  host_ = NULL;
  ResetHost();

  if (policy_watcher_.get()) {
    base::WaitableEvent done_event(true, false);
    policy_watcher_->StopWatching(&done_event);
    done_event.Wait();
    policy_watcher_.reset();
  }

  // Complete the rest of shutdown on the main thread.
  context_->ui_task_runner()->PostTask(
      FROM_HERE,
      base::Bind(&HostProcess::ShutdownHostProcess,
                 base::Unretained(this)));
}

void HostProcess::ResetHost() {
  DCHECK(context_->network_task_runner()->BelongsToCurrentThread());

  curtaining_host_observer_.reset();
  host_event_logger_.reset();
  log_to_server_.reset();
  heartbeat_sender_.reset();
  signaling_connector_.reset();
  signal_strategy_.reset();
  resizing_host_observer_.reset();
}

void HostProcess::OnCrash(const std::string& function_name,
                          const std::string& file_name,
                          const int& line_number) {
  CHECK(false);
}

}  // namespace remoting

int main(int argc, char** argv) {
#if defined(OS_MACOSX)
  // Needed so we don't leak objects when threads are created.
  base::mac::ScopedNSAutoreleasePool pool;
#endif

  CommandLine::Init(argc, argv);

  // This object instance is required by Chrome code (for example,
  // LazyInstance, MessageLoop).
  base::AtExitManager exit_manager;

  if (CommandLine::ForCurrentProcess()->HasSwitch(kVersionSwitchName)) {
    printf("%s\n", STRINGIZE(VERSION));
    return 0;
  }

  remoting::InitHostLogging();

#if defined(TOOLKIT_GTK)
  // Required for any calls into GTK functions, such as the Disconnect and
  // Continue windows, though these should not be used for the Me2Me case
  // (crbug.com/104377).
  const CommandLine* cmd_line = CommandLine::ForCurrentProcess();
  gfx::GtkInitFromCommandLine(*cmd_line);
#endif  // TOOLKIT_GTK

  // Enable support for SSL server sockets, which must be done while still
  // single-threaded.
  net::EnableSSLServerSockets();

  // Create the main message loop and start helper threads.
  MessageLoop message_loop(MessageLoop::TYPE_UI);
  base::Closure quit_message_loop = base::Bind(&QuitMessageLoop, &message_loop);
  scoped_ptr<remoting::ChromotingHostContext> context(
      new remoting::ChromotingHostContext(
          new remoting::AutoThreadTaskRunner(message_loop.message_loop_proxy(),
                                             quit_message_loop)));

#if defined(OS_LINUX)
  // TODO(sergeyu): Pass configuration parameters to the Linux-specific version
  // of DesktopEnvironmentFactory when we have it.
  remoting::VideoFrameCapturer::EnableXDamage(true);
  remoting::AudioCapturerLinux::SetPipeName(CommandLine::ForCurrentProcess()->
      GetSwitchValuePath(kAudioPipeSwitchName));
#endif  // defined(OS_LINUX)

  if (!context->Start())
    return remoting::kInitializationFailed;

  // Create the host process instance and enter the main message loop.
  remoting::HostProcess me2me_host(context.Pass());
  me2me_host.StartHostProcess();
  message_loop.Run();
  return me2me_host.get_exit_code();
}

#if defined(OS_WIN)
HMODULE g_hModule = NULL;

int CALLBACK WinMain(HINSTANCE instance,
                     HINSTANCE previous_instance,
                     LPSTR command_line,
                     int show_command) {
#ifdef OFFICIAL_BUILD
  if (remoting::IsUsageStatsAllowed()) {
    remoting::InitializeCrashReporting();
  }
#endif  // OFFICIAL_BUILD

  g_hModule = instance;

  // Register and initialize common controls.
  INITCOMMONCONTROLSEX info;
  info.dwSize = sizeof(info);
  info.dwICC = ICC_STANDARD_CLASSES;
  InitCommonControlsEx(&info);

  // Mark the process as DPI-aware, so Windows won't scale coordinates in APIs.
  // N.B. This API exists on Vista and above.
  if (base::win::GetVersion() >= base::win::VERSION_VISTA) {
    FilePath path(base::GetNativeLibraryName(UTF8ToUTF16("user32")));
    base::ScopedNativeLibrary user32(path);
    CHECK(user32.is_valid());

    typedef BOOL (WINAPI * SetProcessDPIAwareFn)();
    SetProcessDPIAwareFn set_process_dpi_aware =
        static_cast<SetProcessDPIAwareFn>(
            user32.GetFunctionPointer("SetProcessDPIAware"));
    set_process_dpi_aware();
  }

  // CommandLine::Init() ignores the passed |argc| and |argv| on Windows getting
  // the command line from GetCommandLineW(), so we can safely pass NULL here.
  return main(0, NULL);
}

#endif  // defined(OS_WIN)
