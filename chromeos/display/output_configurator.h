// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMEOS_DISPLAY_OUTPUT_CONFIGURATOR_H_
#define CHROMEOS_DISPLAY_OUTPUT_CONFIGURATOR_H_

#include "base/basictypes.h"
#include "base/event_types.h"
#include "base/observer_list.h"
#include "base/memory/scoped_ptr.h"
#include "base/message_loop.h"
#include "base/timer.h"
#include "chromeos/chromeos_export.h"

// Forward declarations for Xlib and Xrandr.
// This is so unused X definitions don't pollute the namespace.
typedef unsigned long XID;
typedef XID Window;
typedef XID RROutput;
typedef XID RRCrtc;
typedef XID RRMode;

struct _XDisplay;
typedef struct _XDisplay Display;
struct _XRROutputInfo;
typedef _XRROutputInfo XRROutputInfo;
struct _XRRScreenResources;
typedef _XRRScreenResources XRRScreenResources;

namespace chromeos {

struct OutputSnapshot;

// Used to describe the state of a multi-display configuration.
// TODO(oshima): remove DUAL_SECONDARY_ONLY
enum OutputState {
  STATE_INVALID,
  STATE_HEADLESS,
  STATE_SINGLE,
  STATE_DUAL_MIRROR,
  STATE_DUAL_PRIMARY_ONLY,
  STATE_DUAL_SECONDARY_ONLY,
  STATE_DUAL_UNKNOWN,
};

// This class interacts directly with the underlying Xrandr API to manipulate
// CTRCs and Outputs.  It will likely grow more state, over time, or expose
// Output info in other ways as more of the Chrome display code grows up around
// it.
class CHROMEOS_EXPORT OutputConfigurator : public MessageLoop::Dispatcher {
 public:
  class Observer {
   public:
    // Called when the change of the display mode finished.  It will usually
    // start the fading in the displays.
    virtual void OnDisplayModeChanged() {}

    // Called when the change of the display mode is issued but failed.
    virtual void OnDisplayModeChangeFailed() {}
  };

  OutputConfigurator();
  virtual ~OutputConfigurator();

  int connected_output_count() const { return connected_output_count_; }

  OutputState output_state() const { return output_state_; }

  // Initialization, must be called right after constructor.
  // |is_panel_fitting_enabled| indicates hardware panel fitting support.
  void Init(bool is_panel_fitting_enabled);

  // Called when the user hits ctrl-F4 to request a display mode change.
  // This method should only return false if it was called in a single-head or
  // headless mode.
  bool CycleDisplayMode();

  // Called when powerd notifies us that some set of displays should be turned
  // on or off.  This requires enabling or disabling the CRTC associated with
  // the display(s) in question so that the low power state is engaged.
  bool ScreenPowerSet(bool power_on, bool all_displays);

  // Force switching the display mode to |new_state|.  This method is used when
  // the user explicitly changes the display mode in the options UI.  Returns
  // false if it was called in a single-head or headless mode.
  bool SetDisplayMode(OutputState new_state);

  // Called when an RRNotify event is received.  The implementation is
  // interested in the cases of RRNotify events which correspond to output
  // add/remove events.  Note that Output add/remove events are sent in response
  // to our own reconfiguration operations so spurious events are common.
  // Spurious events will have no effect.
  virtual bool Dispatch(const base::NativeEvent& event) OVERRIDE;

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // Tells if the output specified by |name| is for internal display.
  static bool IsInternalOutputName(const std::string& name);

 private:
  // Fires OnDisplayModeChanged() event to the observers.
  void NotifyOnDisplayChanged();

  // Fills output parameters |one| and |two| with properties of
  // first two connected outputs found on |display| and |screen|.
  int GetDualOutputs(Display* display,
                     XRRScreenResources* screen,
                     OutputSnapshot* one,
                     OutputSnapshot* two);

  // Should be called if the internal (built-in) output didn't advertise a mode
  // which would be capable to support mirror mode.
  // Relies on hardware panel fitting support,
  // returns immediately if it is not available.
  // Tries to add the native mode of the external output to the internal output,
  // assuming panel fitter hardware will take care of scaling and letterboxing.
  // The RROutput IDs |output_one| and |output_two| are used
  // to look up the modes and configure the internal output,
  // |output_one_mode| and |output_two_mode| are the out-parameters
  // for the modes on the two outputs which will have same resolution.
  // Returns false if it fails to configure the internal output appropriately.
  bool AddMirrorModeToInternalOutput(Display* display,
                                     XRRScreenResources* screen,
                                     RROutput output_one,
                                     RROutput output_two,
                                     RRMode* output_one_mode,
                                     RRMode* output_two_mode);

  // Tells if the output specified by |output_info| is for internal display.
  static bool IsInternalOutput(const XRROutputInfo* output_info);

  // Returns output's native mode, None if not found.
  static RRMode GetOutputNativeMode(const XRROutputInfo* output_info);

  // This is detected by the constructor to determine whether or not we should
  // be enabled.  If we aren't running on ChromeOS, we can't assume that the
  // Xrandr X11 extension is supported.
  // If this flag is set to false, any attempts to change the output
  // configuration to immediately fail without changing the state.
  bool is_running_on_chrome_os_;

  // This is set externally in Init,
  // and is used to enable modes which rely on panel fitting.
  bool is_panel_fitting_enabled_;

  // The number of outputs that are connected.
  int connected_output_count_;

  // The base of the event numbers used to represent XRandr events used in
  // decoding events regarding output add/remove.
  int xrandr_event_base_;

  // The display state as derived from the outputs observed in |output_cache_|.
  // This is used for rotating display modes.
  OutputState output_state_;

  ObserverList<Observer> observers_;

  // The timer to delay sending the notification of OnDisplayChanged(). See also
  // the comments in Dispatch().
  scoped_ptr<base::OneShotTimer<OutputConfigurator> > notification_timer_;

  DISALLOW_COPY_AND_ASSIGN(OutputConfigurator);
};

}  // namespace chromeos

#endif  // CHROMEOS_DISPLAY_OUTPUT_CONFIGURATOR_H_
