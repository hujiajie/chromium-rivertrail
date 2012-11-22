// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/basictypes.h"
#include "base/memory/scoped_ptr.h"
#include "base/shared_memory.h"
#include "base/timer.h"
#include "content/browser/browser_thread_impl.h"
#include "content/browser/renderer_host/backing_store.h"
#include "content/browser/renderer_host/render_widget_host_delegate.h"
#include "content/browser/renderer_host/gesture_event_filter.h"
#include "content/browser/renderer_host/test_render_view_host.h"
#include "content/browser/renderer_host/touch_event_queue.h"
#include "content/common/view_messages.h"
#include "content/port/browser/render_widget_host_view_port.h"
#include "content/public/browser/notification_details.h"
#include "content/public/browser/notification_observer.h"
#include "content/public/browser/notification_registrar.h"
#include "content/public/browser/notification_source.h"
#include "content/public/browser/notification_types.h"
#include "content/public/test/mock_render_process_host.h"
#include "content/public/test/test_browser_context.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/keycodes/keyboard_codes.h"
#include "ui/gfx/canvas.h"

#if defined(USE_AURA)
#include "content/browser/renderer_host/render_widget_host_view_aura.h"
#include "ui/aura/env.h"
#endif

#if defined(OS_WIN) || defined(USE_AURA)
#include "content/browser/renderer_host/ui_events_helper.h"
#include "ui/base/events/event.h"
#endif

using base::TimeDelta;
using WebKit::WebGestureEvent;
using WebKit::WebInputEvent;
using WebKit::WebMouseWheelEvent;
using WebKit::WebTouchEvent;
using WebKit::WebTouchPoint;

namespace content {

#if defined(OS_WIN) || defined(USE_AURA)
bool TouchEventsAreEquivalent(const ui::TouchEvent& first,
                              const ui::TouchEvent& second) {
  EXPECT_EQ(second.type(), first.type());
  if (first.type() != second.type())
    return false;
  EXPECT_EQ(second.location().ToString(), first.location().ToString());
  if (first.location() != second.location())
    return false;
  EXPECT_EQ(second.touch_id(), first.touch_id());
  if (first.touch_id() != second.touch_id())
    return false;
  EXPECT_EQ(second.time_stamp().InSeconds(), first.time_stamp().InSeconds());
  if (second.time_stamp().InSeconds() != first.time_stamp().InSeconds())
    return false;
  return true;
}

bool EventListIsSubset(const ScopedVector<ui::TouchEvent>& subset,
                       const ScopedVector<ui::TouchEvent>& set) {
  EXPECT_LE(subset.size(), set.size());
  if (subset.size() > set.size())
    return false;
  for (size_t i = 0; i < subset.size(); ++i) {
    const ui::TouchEvent* first = subset[i];
    const ui::TouchEvent* second = set[i];
    bool equivalent = TouchEventsAreEquivalent(*first, *second);
    EXPECT_TRUE(equivalent);
    if (!equivalent)
      return false;
  }

  return true;
}
#endif  // defined(OS_WIN) || defined(USE_AURA)

// RenderWidgetHostProcess -----------------------------------------------------

class RenderWidgetHostProcess : public MockRenderProcessHost {
 public:
  explicit RenderWidgetHostProcess(BrowserContext* browser_context)
      : MockRenderProcessHost(browser_context),
        current_update_buf_(NULL),
        update_msg_should_reply_(false),
        update_msg_reply_flags_(0) {
  }
  ~RenderWidgetHostProcess() {
    delete current_update_buf_;
  }

  void set_update_msg_should_reply(bool reply) {
    update_msg_should_reply_ = reply;
  }
  void set_update_msg_reply_flags(int flags) {
    update_msg_reply_flags_ = flags;
  }

  // Fills the given update parameters with resonable default values.
  void InitUpdateRectParams(ViewHostMsg_UpdateRect_Params* params);

  virtual bool HasConnection() const { return true; }

 protected:
  virtual bool WaitForBackingStoreMsg(int render_widget_id,
                                      const base::TimeDelta& max_delay,
                                      IPC::Message* msg);

  TransportDIB* current_update_buf_;

  // Set to true when WaitForBackingStoreMsg should return a successful update
  // message reply. False implies timeout.
  bool update_msg_should_reply_;

  // Indicates the flags that should be sent with a repaint request. This
  // only has an effect when update_msg_should_reply_ is true.
  int update_msg_reply_flags_;

  DISALLOW_COPY_AND_ASSIGN(RenderWidgetHostProcess);
};

void RenderWidgetHostProcess::InitUpdateRectParams(
    ViewHostMsg_UpdateRect_Params* params) {
  // Create the shared backing store.
  const int w = 100, h = 100;
  const size_t pixel_size = w * h * 4;

  if (!current_update_buf_)
    current_update_buf_ = TransportDIB::Create(pixel_size, 0);
  params->bitmap = current_update_buf_->id();
  params->bitmap_rect = gfx::Rect(0, 0, w, h);
  params->dx = 0;
  params->dy = 0;
  params->copy_rects.push_back(params->bitmap_rect);
  params->view_size = gfx::Size(w, h);
  params->flags = update_msg_reply_flags_;
  params->needs_ack = true;
  params->scale_factor = 1;
}

bool RenderWidgetHostProcess::WaitForBackingStoreMsg(
    int render_widget_id,
    const base::TimeDelta& max_delay,
    IPC::Message* msg) {
  if (!update_msg_should_reply_)
    return false;

  // Construct a fake update reply.
  ViewHostMsg_UpdateRect_Params params;
  InitUpdateRectParams(&params);

  ViewHostMsg_UpdateRect message(render_widget_id, params);
  *msg = message;
  return true;
}

// TestView --------------------------------------------------------------------

// This test view allows us to specify the size, and keep track of acked
// touch-events.
class TestView : public TestRenderWidgetHostView {
 public:
  explicit TestView(RenderWidgetHostImpl* rwh)
      : TestRenderWidgetHostView(rwh),
        acked_event_count_(0) {
  }

  // Sets the bounds returned by GetViewBounds.
  void set_bounds(const gfx::Rect& bounds) {
    bounds_ = bounds;
  }

  const WebTouchEvent& acked_event() const { return acked_event_; }
  int acked_event_count() const { return acked_event_count_; }
  void ClearAckedEvent() {
    acked_event_.type = WebKit::WebInputEvent::Undefined;
    acked_event_count_ = 0;
  }

  // RenderWidgetHostView override.
  virtual gfx::Rect GetViewBounds() const OVERRIDE {
    return bounds_;
  }
  virtual void ProcessAckedTouchEvent(const WebTouchEvent& touch,
                                      bool processed) OVERRIDE {
    acked_event_ = touch;
    ++acked_event_count_;
  }

 protected:
  WebTouchEvent acked_event_;
  int acked_event_count_;
  gfx::Rect bounds_;

  DISALLOW_COPY_AND_ASSIGN(TestView);
};

// MockRenderWidgetHostDelegate --------------------------------------------

class MockRenderWidgetHostDelegate : public RenderWidgetHostDelegate {
 public:
  MockRenderWidgetHostDelegate()
      : prehandle_keyboard_event_(false),
        prehandle_keyboard_event_called_(false),
        prehandle_keyboard_event_type_(WebInputEvent::Undefined),
        unhandled_keyboard_event_called_(false),
        unhandled_keyboard_event_type_(WebInputEvent::Undefined) {
  }
  virtual ~MockRenderWidgetHostDelegate() {}

  // Tests that make sure we ignore keyboard event acknowledgments to events we
  // didn't send work by making sure we didn't call UnhandledKeyboardEvent().
  bool unhandled_keyboard_event_called() const {
    return unhandled_keyboard_event_called_;
  }

  WebInputEvent::Type unhandled_keyboard_event_type() const {
    return unhandled_keyboard_event_type_;
  }

  bool prehandle_keyboard_event_called() const {
    return prehandle_keyboard_event_called_;
  }

  WebInputEvent::Type prehandle_keyboard_event_type() const {
    return prehandle_keyboard_event_type_;
  }

  void set_prehandle_keyboard_event(bool handle) {
    prehandle_keyboard_event_ = handle;
  }

 protected:
  virtual bool PreHandleKeyboardEvent(const NativeWebKeyboardEvent& event,
                                      bool* is_keyboard_shortcut) OVERRIDE {
    prehandle_keyboard_event_type_ = event.type;
    prehandle_keyboard_event_called_ = true;
    return prehandle_keyboard_event_;
  }

  virtual void HandleKeyboardEvent(
      const NativeWebKeyboardEvent& event) OVERRIDE {
    unhandled_keyboard_event_type_ = event.type;
    unhandled_keyboard_event_called_ = true;
  }

 private:
  bool prehandle_keyboard_event_;
  bool prehandle_keyboard_event_called_;
  WebInputEvent::Type prehandle_keyboard_event_type_;

  bool unhandled_keyboard_event_called_;
  WebInputEvent::Type unhandled_keyboard_event_type_;
};

// MockRenderWidgetHost ----------------------------------------------------

class MockRenderWidgetHost : public RenderWidgetHostImpl {
 public:
  MockRenderWidgetHost(
      RenderWidgetHostDelegate* delegate,
      RenderProcessHost* process,
      int routing_id)
      : RenderWidgetHostImpl(delegate, process, routing_id),
        unresponsive_timer_fired_(false) {
  }

  // Allow poking at a few private members.
  using RenderWidgetHostImpl::OnMsgPaintAtSizeAck;
  using RenderWidgetHostImpl::OnMsgUpdateRect;
  using RenderWidgetHostImpl::RendererExited;
  using RenderWidgetHostImpl::in_flight_size_;
  using RenderWidgetHostImpl::is_hidden_;
  using RenderWidgetHostImpl::resize_ack_pending_;
  using RenderWidgetHostImpl::gesture_event_filter_;
  using RenderWidgetHostImpl::touch_event_queue_;

  bool unresponsive_timer_fired() const {
    return unresponsive_timer_fired_;
  }

  void set_hung_renderer_delay_ms(int delay_ms) {
    hung_renderer_delay_ms_ = delay_ms;
  }

  WebGestureEvent GestureEventLastQueueEvent() {
    return gesture_event_filter_->coalesced_gesture_events_.back();
  }

  unsigned GestureEventLastQueueEventSize() {
    return gesture_event_filter_->coalesced_gesture_events_.size();
  }

  unsigned GestureEventDebouncingQueueSize() {
    return gesture_event_filter_->debouncing_deferral_queue_.size();
  }

  WebGestureEvent GestureEventQueueEventAt(int i) {
    return gesture_event_filter_->coalesced_gesture_events_.at(i);
  }

  bool ScrollingInProgress() {
    return gesture_event_filter_->scrolling_in_progress_;
  }

  bool FlingInProgress() {
    return gesture_event_filter_->fling_in_progress_;
  }

  void set_maximum_tap_gap_time_ms(int delay_ms) {
    gesture_event_filter_->maximum_tap_gap_time_ms_ = delay_ms;
  }

  void set_debounce_interval_time_ms(int delay_ms) {
    gesture_event_filter_->debounce_interval_time_ms_ = delay_ms;
  }

  size_t TouchEventQueueSize() {
    return touch_event_queue_->GetQueueSize();
  }

  const WebTouchEvent& latest_event() const {
    return touch_event_queue_->GetLatestEvent();
  }

 protected:
  virtual void NotifyRendererUnresponsive() OVERRIDE {
    unresponsive_timer_fired_ = true;
  }

 private:
  bool unresponsive_timer_fired_;
};

// MockPaintingObserver --------------------------------------------------------

class MockPaintingObserver : public NotificationObserver {
 public:
  void WidgetDidReceivePaintAtSizeAck(RenderWidgetHostImpl* host,
                                      int tag,
                                      const gfx::Size& size) {
    host_ = reinterpret_cast<MockRenderWidgetHost*>(host);
    tag_ = tag;
    size_ = size;
  }

  void Observe(int type,
               const NotificationSource& source,
               const NotificationDetails& details) {
    if (type == NOTIFICATION_RENDER_WIDGET_HOST_DID_RECEIVE_PAINT_AT_SIZE_ACK) {
      std::pair<int, gfx::Size>* size_ack_details =
          Details<std::pair<int, gfx::Size> >(details).ptr();
      WidgetDidReceivePaintAtSizeAck(
          RenderWidgetHostImpl::From(Source<RenderWidgetHost>(source).ptr()),
          size_ack_details->first,
          size_ack_details->second);
    }
  }

  MockRenderWidgetHost* host() const { return host_; }
  int tag() const { return tag_; }
  gfx::Size size() const { return size_; }

 private:
  MockRenderWidgetHost* host_;
  int tag_;
  gfx::Size size_;
};


// RenderWidgetHostTest --------------------------------------------------------

class RenderWidgetHostTest : public testing::Test {
 public:
  RenderWidgetHostTest() : process_(NULL) {
  }
  ~RenderWidgetHostTest() {
  }

 protected:
  // testing::Test
  void SetUp() {
    browser_context_.reset(new TestBrowserContext());
    delegate_.reset(new MockRenderWidgetHostDelegate());
    process_ = new RenderWidgetHostProcess(browser_context_.get());
    host_.reset(
        new MockRenderWidgetHost(delegate_.get(), process_, MSG_ROUTING_NONE));
    view_.reset(new TestView(host_.get()));
    host_->SetView(view_.get());
    host_->Init();
  }
  void TearDown() {
    view_.reset();
    host_.reset();
    delegate_.reset();
    process_ = NULL;
    browser_context_.reset();

#if defined(USE_AURA)
    aura::Env::DeleteInstance();
#endif

    // Process all pending tasks to avoid leaks.
    MessageLoop::current()->RunAllPending();
  }

  void SendInputEventACK(WebInputEvent::Type type, bool processed) {
    scoped_ptr<IPC::Message> response(
        new ViewHostMsg_HandleInputEvent_ACK(0, type, processed));
    host_->OnMessageReceived(*response);
  }

  void SimulateKeyboardEvent(WebInputEvent::Type type) {
    NativeWebKeyboardEvent key_event;
    key_event.type = type;
    key_event.windowsKeyCode = ui::VKEY_L;  // non-null made up value.
    host_->ForwardKeyboardEvent(key_event);
  }

  void SimulateWheelEvent(float dX, float dY, int modifiers) {
    WebMouseWheelEvent wheel_event;
    wheel_event.type = WebInputEvent::MouseWheel;
    wheel_event.deltaX = dX;
    wheel_event.deltaY = dY;
    wheel_event.modifiers = modifiers;
    host_->ForwardWheelEvent(wheel_event);
  }

  // Inject simple synthetic WebGestureEvent instances.
  void SimulateGestureEvent(WebInputEvent::Type type) {
    WebGestureEvent gesture_event;
    gesture_event.type = type;
    host_->ForwardGestureEvent(gesture_event);
  }

  void SimulateGestureScrollUpdateEvent(float dX, float dY, int modifiers) {
    WebGestureEvent gesture_event;
    gesture_event.type = WebInputEvent::GestureScrollUpdate;
    gesture_event.data.scrollUpdate.deltaX = dX;
    gesture_event.data.scrollUpdate.deltaY = dY;
    gesture_event.modifiers = modifiers;
    host_->ForwardGestureEvent(gesture_event);
  }

  // Inject simple synthetic WebGestureEvent instances.
  void SimulateGestureFlingStartEvent(float velocityX, float velocityY) {
    WebGestureEvent gesture_event;
    gesture_event.type = WebInputEvent::GestureFlingStart;
    gesture_event.data.flingStart.velocityX = velocityX;
    gesture_event.data.flingStart.velocityY = velocityY;
    host_->ForwardGestureEvent(gesture_event);
  }

  // Set the timestamp for the touch-event.
  void SetTouchTimestamp(base::TimeDelta timestamp) {
    touch_event_.timeStampSeconds = timestamp.InSecondsF();
  }

  // Sends a touch event (irrespective of whether the page has a touch-event
  // handler or not).
  void SendTouchEvent() {
    host_->ForwardTouchEvent(touch_event_);

    // Mark all the points as stationary. And remove the points that have been
    // released.
    int point = 0;
    for (unsigned int i = 0; i < touch_event_.touchesLength; ++i) {
      if (touch_event_.touches[i].state == WebKit::WebTouchPoint::StateReleased)
        continue;

      touch_event_.touches[point] = touch_event_.touches[i];
      touch_event_.touches[point].state =
          WebKit::WebTouchPoint::StateStationary;
      ++point;
    }
    touch_event_.touchesLength = point;
    touch_event_.type = WebInputEvent::Undefined;
  }

  int PressTouchPoint(int x, int y) {
    if (touch_event_.touchesLength == touch_event_.touchesLengthCap)
      return -1;
    WebKit::WebTouchPoint& point =
        touch_event_.touches[touch_event_.touchesLength];
    point.id = touch_event_.touchesLength;
    point.position.x = point.screenPosition.x = x;
    point.position.y = point.screenPosition.y = y;
    point.state = WebKit::WebTouchPoint::StatePressed;
    point.radiusX = point.radiusY = 1.f;
    ++touch_event_.touchesLength;
    touch_event_.type = WebInputEvent::TouchStart;
    return point.id;
  }

  void MoveTouchPoint(int index, int x, int y) {
    CHECK(index >= 0 && index < touch_event_.touchesLengthCap);
    WebKit::WebTouchPoint& point = touch_event_.touches[index];
    point.position.x = point.screenPosition.x = x;
    point.position.y = point.screenPosition.y = y;
    touch_event_.touches[index].state = WebKit::WebTouchPoint::StateMoved;
    touch_event_.type = WebInputEvent::TouchMove;
  }

  void ReleaseTouchPoint(int index) {
    CHECK(index >= 0 && index < touch_event_.touchesLengthCap);
    touch_event_.touches[index].state = WebKit::WebTouchPoint::StateReleased;
    touch_event_.type = WebInputEvent::TouchEnd;
  }

  MessageLoopForUI message_loop_;

  scoped_ptr<TestBrowserContext> browser_context_;
  RenderWidgetHostProcess* process_;  // Deleted automatically by the widget.
  scoped_ptr<MockRenderWidgetHostDelegate> delegate_;
  scoped_ptr<MockRenderWidgetHost> host_;
  scoped_ptr<TestView> view_;

 private:
  WebTouchEvent touch_event_;

  DISALLOW_COPY_AND_ASSIGN(RenderWidgetHostTest);
};

// -----------------------------------------------------------------------------

TEST_F(RenderWidgetHostTest, Resize) {
  // The initial bounds is the empty rect, so setting it to the same thing
  // should do nothing.
  view_->set_bounds(gfx::Rect());
  host_->WasResized();
  EXPECT_FALSE(host_->resize_ack_pending_);
  EXPECT_EQ(gfx::Size(), host_->in_flight_size_);
  EXPECT_FALSE(process_->sink().GetUniqueMessageMatching(ViewMsg_Resize::ID));

  // Setting the bounds to a "real" rect should send out the notification.
  gfx::Rect original_size(0, 0, 100, 100);
  process_->sink().ClearMessages();
  view_->set_bounds(original_size);
  host_->WasResized();
  EXPECT_TRUE(host_->resize_ack_pending_);
  EXPECT_EQ(original_size.size(), host_->in_flight_size_);
  EXPECT_TRUE(process_->sink().GetUniqueMessageMatching(ViewMsg_Resize::ID));

  // Send out a update that's not a resize ack. This should not clean the
  // resize ack pending flag.
  ViewHostMsg_UpdateRect_Params params;
  process_->InitUpdateRectParams(&params);
  host_->OnMsgUpdateRect(params);
  EXPECT_TRUE(host_->resize_ack_pending_);
  EXPECT_EQ(original_size.size(), host_->in_flight_size_);

  // Sending out a new notification should NOT send out a new IPC message since
  // a resize ACK is pending.
  gfx::Rect second_size(0, 0, 90, 90);
  process_->sink().ClearMessages();
  view_->set_bounds(second_size);
  host_->WasResized();
  EXPECT_TRUE(host_->resize_ack_pending_);
  EXPECT_EQ(original_size.size(), host_->in_flight_size_);
  EXPECT_FALSE(process_->sink().GetUniqueMessageMatching(ViewMsg_Resize::ID));

  // Send a update that's a resize ack, but for the original_size we sent. Since
  // this isn't the second_size, the message handler should immediately send
  // a new resize message for the new size to the renderer.
  process_->sink().ClearMessages();
  params.flags = ViewHostMsg_UpdateRect_Flags::IS_RESIZE_ACK;
  params.view_size = original_size.size();
  host_->OnMsgUpdateRect(params);
  EXPECT_TRUE(host_->resize_ack_pending_);
  EXPECT_EQ(second_size.size(), host_->in_flight_size_);
  ASSERT_TRUE(process_->sink().GetUniqueMessageMatching(ViewMsg_Resize::ID));

  // Send the resize ack for the latest size.
  process_->sink().ClearMessages();
  params.view_size = second_size.size();
  host_->OnMsgUpdateRect(params);
  EXPECT_FALSE(host_->resize_ack_pending_);
  EXPECT_EQ(gfx::Size(), host_->in_flight_size_);
  ASSERT_FALSE(process_->sink().GetFirstMessageMatching(ViewMsg_Resize::ID));

  // Now clearing the bounds should send out a notification but we shouldn't
  // expect a resize ack (since the renderer won't ack empty sizes). The message
  // should contain the new size (0x0) and not the previous one that we skipped
  process_->sink().ClearMessages();
  view_->set_bounds(gfx::Rect());
  host_->WasResized();
  EXPECT_FALSE(host_->resize_ack_pending_);
  EXPECT_EQ(gfx::Size(), host_->in_flight_size_);
  EXPECT_TRUE(process_->sink().GetUniqueMessageMatching(ViewMsg_Resize::ID));

  // Send a rect that has no area but has either width or height set.
  process_->sink().ClearMessages();
  view_->set_bounds(gfx::Rect(0, 0, 0, 30));
  host_->WasResized();
  EXPECT_FALSE(host_->resize_ack_pending_);
  EXPECT_EQ(gfx::Size(0, 30), host_->in_flight_size_);
  EXPECT_TRUE(process_->sink().GetUniqueMessageMatching(ViewMsg_Resize::ID));

  // Set the same size again. It should not be sent again.
  process_->sink().ClearMessages();
  host_->WasResized();
  EXPECT_FALSE(host_->resize_ack_pending_);
  EXPECT_EQ(gfx::Size(0, 30), host_->in_flight_size_);
  EXPECT_FALSE(process_->sink().GetFirstMessageMatching(ViewMsg_Resize::ID));

  // A different size should be sent again, however.
  view_->set_bounds(gfx::Rect(0, 0, 0, 31));
  host_->WasResized();
  EXPECT_FALSE(host_->resize_ack_pending_);
  EXPECT_EQ(gfx::Size(0, 31), host_->in_flight_size_);
  EXPECT_TRUE(process_->sink().GetUniqueMessageMatching(ViewMsg_Resize::ID));
}

// Test for crbug.com/25097.  If a renderer crashes between a resize and the
// corresponding update message, we must be sure to clear the resize ack logic.
TEST_F(RenderWidgetHostTest, ResizeThenCrash) {
  // Setting the bounds to a "real" rect should send out the notification.
  gfx::Rect original_size(0, 0, 100, 100);
  view_->set_bounds(original_size);
  host_->WasResized();
  EXPECT_TRUE(host_->resize_ack_pending_);
  EXPECT_EQ(original_size.size(), host_->in_flight_size_);
  EXPECT_TRUE(process_->sink().GetUniqueMessageMatching(ViewMsg_Resize::ID));

  // Simulate a renderer crash before the update message.  Ensure all the
  // resize ack logic is cleared.  Must clear the view first so it doesn't get
  // deleted.
  host_->SetView(NULL);
  host_->RendererExited(base::TERMINATION_STATUS_PROCESS_CRASHED, -1);
  EXPECT_FALSE(host_->resize_ack_pending_);
  EXPECT_EQ(gfx::Size(), host_->in_flight_size_);

  // Reset the view so we can exit the test cleanly.
  host_->SetView(view_.get());
}

// Tests setting custom background
TEST_F(RenderWidgetHostTest, Background) {
#if !defined(OS_MACOSX)
  scoped_ptr<RenderWidgetHostView> view(
      RenderWidgetHostView::CreateViewForWidget(host_.get()));
#if defined(OS_LINUX) || defined(USE_AURA)
  // TODO(derat): Call this on all platforms: http://crbug.com/102450.
  // InitAsChild doesn't seem to work if NULL parent is passed on Windows,
  // which leads to DCHECK failure in RenderWidgetHostView::Destroy.
  // When you enable this for OS_WIN, enable |view.release()->Destroy()|
  // below.
  view->InitAsChild(NULL);
#endif
  host_->SetView(view.get());

  // Create a checkerboard background to test with.
  gfx::Canvas canvas(gfx::Size(4, 4), ui::SCALE_FACTOR_100P, true);
  canvas.FillRect(gfx::Rect(0, 0, 2, 2), SK_ColorBLACK);
  canvas.FillRect(gfx::Rect(2, 0, 2, 2), SK_ColorWHITE);
  canvas.FillRect(gfx::Rect(0, 2, 2, 2), SK_ColorWHITE);
  canvas.FillRect(gfx::Rect(2, 2, 2, 2), SK_ColorBLACK);
  const SkBitmap& background =
      canvas.sk_canvas()->getDevice()->accessBitmap(false);

  // Set the background and make sure we get back a copy.
  view->SetBackground(background);
  EXPECT_EQ(4, view->GetBackground().width());
  EXPECT_EQ(4, view->GetBackground().height());
  EXPECT_EQ(background.getSize(), view->GetBackground().getSize());
  background.lockPixels();
  view->GetBackground().lockPixels();
  EXPECT_TRUE(0 == memcmp(background.getPixels(),
                          view->GetBackground().getPixels(),
                          background.getSize()));
  view->GetBackground().unlockPixels();
  background.unlockPixels();

  const IPC::Message* set_background =
      process_->sink().GetUniqueMessageMatching(ViewMsg_SetBackground::ID);
  ASSERT_TRUE(set_background);
  Tuple1<SkBitmap> sent_background;
  ViewMsg_SetBackground::Read(set_background, &sent_background);
  EXPECT_EQ(background.getSize(), sent_background.a.getSize());
  background.lockPixels();
  sent_background.a.lockPixels();
  EXPECT_TRUE(0 == memcmp(background.getPixels(),
                          sent_background.a.getPixels(),
                          background.getSize()));
  sent_background.a.unlockPixels();
  background.unlockPixels();

#if defined(OS_LINUX) || defined(USE_AURA)
  // See the comment above |InitAsChild(NULL)|.
  host_->SetView(NULL);
  static_cast<RenderWidgetHostViewPort*>(view.release())->Destroy();
#endif

#else
  // TODO(port): Mac does not have gfx::Canvas. Maybe we can just change this
  // test to use SkCanvas directly?
#endif

  // TODO(aa): It would be nice to factor out the painting logic so that we
  // could test that, but it appears that would mean painting everything twice
  // since windows HDC structures are opaque.
}

// Tests getting the backing store with the renderer not setting repaint ack
// flags.
TEST_F(RenderWidgetHostTest, GetBackingStore_NoRepaintAck) {
  // First set the view size to match what the renderer is rendering.
  ViewHostMsg_UpdateRect_Params params;
  process_->InitUpdateRectParams(&params);
  view_->set_bounds(gfx::Rect(params.view_size));

  // We don't currently have a backing store, and if the renderer doesn't send
  // one in time, we should get nothing.
  process_->set_update_msg_should_reply(false);
  BackingStore* backing = host_->GetBackingStore(true);
  EXPECT_FALSE(backing);
  // The widget host should have sent a request for a repaint, and there should
  // be no paint ACK.
  EXPECT_TRUE(process_->sink().GetUniqueMessageMatching(ViewMsg_Repaint::ID));
  EXPECT_FALSE(process_->sink().GetUniqueMessageMatching(
      ViewMsg_UpdateRect_ACK::ID));

  // Allowing the renderer to reply in time should give is a backing store.
  process_->sink().ClearMessages();
  process_->set_update_msg_should_reply(true);
  process_->set_update_msg_reply_flags(0);
  backing = host_->GetBackingStore(true);
  EXPECT_TRUE(backing);
  // The widget host should NOT have sent a request for a repaint, since there
  // was an ACK already pending.
  EXPECT_FALSE(process_->sink().GetUniqueMessageMatching(ViewMsg_Repaint::ID));
  EXPECT_TRUE(process_->sink().GetUniqueMessageMatching(
      ViewMsg_UpdateRect_ACK::ID));
}

// Tests getting the backing store with the renderer sending a repaint ack.
TEST_F(RenderWidgetHostTest, GetBackingStore_RepaintAck) {
  // First set the view size to match what the renderer is rendering.
  ViewHostMsg_UpdateRect_Params params;
  process_->InitUpdateRectParams(&params);
  view_->set_bounds(gfx::Rect(params.view_size));

  // Doing a request request with the update message allowed should work and
  // the repaint ack should work.
  process_->set_update_msg_should_reply(true);
  process_->set_update_msg_reply_flags(
      ViewHostMsg_UpdateRect_Flags::IS_REPAINT_ACK);
  BackingStore* backing = host_->GetBackingStore(true);
  EXPECT_TRUE(backing);
  // We still should not have sent out a repaint request since the last flags
  // didn't have the repaint ack set, and the pending flag will still be set.
  EXPECT_TRUE(process_->sink().GetUniqueMessageMatching(ViewMsg_Repaint::ID));
  EXPECT_TRUE(process_->sink().GetUniqueMessageMatching(
      ViewMsg_UpdateRect_ACK::ID));

  // Asking again for the backing store should just re-use the existing one
  // and not send any messagse.
  process_->sink().ClearMessages();
  backing = host_->GetBackingStore(true);
  EXPECT_TRUE(backing);
  EXPECT_FALSE(process_->sink().GetUniqueMessageMatching(ViewMsg_Repaint::ID));
  EXPECT_FALSE(process_->sink().GetUniqueMessageMatching(
      ViewMsg_UpdateRect_ACK::ID));
}

// Test that we don't paint when we're hidden, but we still send the ACK. Most
// of the rest of the painting is tested in the GetBackingStore* ones.
TEST_F(RenderWidgetHostTest, HiddenPaint) {
  BrowserThreadImpl ui_thread(BrowserThread::UI, MessageLoop::current());
  // Hide the widget, it should have sent out a message to the renderer.
  EXPECT_FALSE(host_->is_hidden_);
  host_->WasHidden();
  EXPECT_TRUE(host_->is_hidden_);
  EXPECT_TRUE(process_->sink().GetUniqueMessageMatching(ViewMsg_WasHidden::ID));

  // Send it an update as from the renderer.
  process_->sink().ClearMessages();
  ViewHostMsg_UpdateRect_Params params;
  process_->InitUpdateRectParams(&params);
  host_->OnMsgUpdateRect(params);

  // It should have sent out the ACK.
  EXPECT_TRUE(process_->sink().GetUniqueMessageMatching(
      ViewMsg_UpdateRect_ACK::ID));

  // Now unhide.
  process_->sink().ClearMessages();
  host_->WasShown();
  EXPECT_FALSE(host_->is_hidden_);

  // It should have sent out a restored message with a request to paint.
  const IPC::Message* restored = process_->sink().GetUniqueMessageMatching(
      ViewMsg_WasShown::ID);
  ASSERT_TRUE(restored);
  Tuple1<bool> needs_repaint;
  ViewMsg_WasShown::Read(restored, &needs_repaint);
  EXPECT_TRUE(needs_repaint.a);
}

TEST_F(RenderWidgetHostTest, PaintAtSize) {
  const int kPaintAtSizeTag = 42;
  host_->PaintAtSize(TransportDIB::GetFakeHandleForTest(), kPaintAtSizeTag,
                     gfx::Size(40, 60), gfx::Size(20, 30));
  EXPECT_TRUE(
      process_->sink().GetUniqueMessageMatching(ViewMsg_PaintAtSize::ID));

  NotificationRegistrar registrar;
  MockPaintingObserver observer;
  registrar.Add(
      &observer,
      NOTIFICATION_RENDER_WIDGET_HOST_DID_RECEIVE_PAINT_AT_SIZE_ACK,
      Source<RenderWidgetHost>(host_.get()));

  host_->OnMsgPaintAtSizeAck(kPaintAtSizeTag, gfx::Size(20, 30));
  EXPECT_EQ(host_.get(), observer.host());
  EXPECT_EQ(kPaintAtSizeTag, observer.tag());
  EXPECT_EQ(20, observer.size().width());
  EXPECT_EQ(30, observer.size().height());
}

// Fails on Linux Aura, see http://crbug.com/100344
#if defined(USE_AURA) && !defined(OS_WIN)
#define MAYBE_HandleKeyEventsWeSent FAILS_HandleKeyEventsWeSent
#else
#define MAYBE_HandleKeyEventsWeSent HandleKeyEventsWeSent
#endif
TEST_F(RenderWidgetHostTest, MAYBE_HandleKeyEventsWeSent) {
  // Simulate a keyboard event.
  SimulateKeyboardEvent(WebInputEvent::RawKeyDown);

  // Make sure we sent the input event to the renderer.
  EXPECT_TRUE(process_->sink().GetUniqueMessageMatching(
                  ViewMsg_HandleInputEvent::ID));
  process_->sink().ClearMessages();

  // Send the simulated response from the renderer back.
  SendInputEventACK(WebInputEvent::RawKeyDown, false);

  EXPECT_TRUE(delegate_->unhandled_keyboard_event_called());
  EXPECT_EQ(WebInputEvent::RawKeyDown,
            delegate_->unhandled_keyboard_event_type());
}

TEST_F(RenderWidgetHostTest, IgnoreKeyEventsWeDidntSend) {
  // Send a simulated, unrequested key response. We should ignore this.
  SendInputEventACK(WebInputEvent::RawKeyDown, false);

  EXPECT_FALSE(delegate_->unhandled_keyboard_event_called());
}

TEST_F(RenderWidgetHostTest, IgnoreKeyEventsHandledByRenderer) {
  // Simulate a keyboard event.
  SimulateKeyboardEvent(WebInputEvent::RawKeyDown);

  // Make sure we sent the input event to the renderer.
  EXPECT_TRUE(process_->sink().GetUniqueMessageMatching(
                  ViewMsg_HandleInputEvent::ID));
  process_->sink().ClearMessages();

  // Send the simulated response from the renderer back.
  SendInputEventACK(WebInputEvent::RawKeyDown, true);
  EXPECT_FALSE(delegate_->unhandled_keyboard_event_called());
}

TEST_F(RenderWidgetHostTest, PreHandleRawKeyDownEvent) {
  // Simluate the situation that the browser handled the key down event during
  // pre-handle phrase.
  delegate_->set_prehandle_keyboard_event(true);
  process_->sink().ClearMessages();

  // Simulate a keyboard event.
  SimulateKeyboardEvent(WebInputEvent::RawKeyDown);

  EXPECT_TRUE(delegate_->prehandle_keyboard_event_called());
  EXPECT_EQ(WebInputEvent::RawKeyDown,
            delegate_->prehandle_keyboard_event_type());

  // Make sure the RawKeyDown event is not sent to the renderer.
  EXPECT_EQ(0U, process_->sink().message_count());

  // The browser won't pre-handle a Char event.
  delegate_->set_prehandle_keyboard_event(false);

  // Forward the Char event.
  SimulateKeyboardEvent(WebInputEvent::Char);

  // Make sure the Char event is suppressed.
  EXPECT_EQ(0U, process_->sink().message_count());

  // Forward the KeyUp event.
  SimulateKeyboardEvent(WebInputEvent::KeyUp);

  // Make sure only KeyUp was sent to the renderer.
  EXPECT_EQ(1U, process_->sink().message_count());
  EXPECT_EQ(ViewMsg_HandleInputEvent::ID,
            process_->sink().GetMessageAt(0)->type());
  process_->sink().ClearMessages();

  // Send the simulated response from the renderer back.
  SendInputEventACK(WebInputEvent::KeyUp, false);

  EXPECT_TRUE(delegate_->unhandled_keyboard_event_called());
  EXPECT_EQ(WebInputEvent::KeyUp, delegate_->unhandled_keyboard_event_type());
}

TEST_F(RenderWidgetHostTest, CoalescesWheelEvents) {
  process_->sink().ClearMessages();

  // Simulate wheel events.
  SimulateWheelEvent(0, -5, 0);  // sent directly
  SimulateWheelEvent(0, -10, 0);  // enqueued
  SimulateWheelEvent(8, -6, 0);  // coalesced into previous event
  SimulateWheelEvent(9, -7, 1);  // enqueued, different modifiers

  // Check that only the first event was sent.
  EXPECT_EQ(1U, process_->sink().message_count());
  EXPECT_TRUE(process_->sink().GetUniqueMessageMatching(
                  ViewMsg_HandleInputEvent::ID));
  process_->sink().ClearMessages();

  // Check that the ACK sends the second message.
  SendInputEventACK(WebInputEvent::MouseWheel, true);
  // The coalesced events can queue up a delayed ack
  // so that additional input events can be processed before
  // we turn off coalescing.
  MessageLoop::current()->RunAllPending();
  EXPECT_EQ(1U, process_->sink().message_count());
  EXPECT_TRUE(process_->sink().GetUniqueMessageMatching(
                  ViewMsg_HandleInputEvent::ID));
  process_->sink().ClearMessages();

  // One more time.
  SendInputEventACK(WebInputEvent::MouseWheel, true);
  MessageLoop::current()->RunAllPending();
  EXPECT_EQ(1U, process_->sink().message_count());
  EXPECT_TRUE(process_->sink().GetUniqueMessageMatching(
                  ViewMsg_HandleInputEvent::ID));
  process_->sink().ClearMessages();

  // After the final ack, the queue should be empty.
  SendInputEventACK(WebInputEvent::MouseWheel, true);
  MessageLoop::current()->RunAllPending();
  EXPECT_EQ(0U, process_->sink().message_count());
}

TEST_F(RenderWidgetHostTest, CoalescesGesturesEvents) {
  // Turn off debounce handling for test isolation.
  host_->set_debounce_interval_time_ms(0);
  process_->sink().ClearMessages();
  // Only GestureScrollUpdate events can be coalesced.
  // Simulate gesture events.

  // Sent.
  SimulateGestureEvent(WebInputEvent::GestureScrollBegin);

  // Enqueued.
  SimulateGestureScrollUpdateEvent(8, -5, 0);

  // Make sure that the queue contains what we think it should.
  WebGestureEvent merged_event = host_->GestureEventLastQueueEvent();
  EXPECT_EQ(2U, host_->GestureEventLastQueueEventSize());
  EXPECT_EQ(WebInputEvent::GestureScrollUpdate, merged_event.type);

  // Coalesced.
  SimulateGestureScrollUpdateEvent(8, -6, 0);

  // Check that coalescing updated the correct values.
  merged_event = host_->GestureEventLastQueueEvent();
  EXPECT_EQ(WebInputEvent::GestureScrollUpdate, merged_event.type);
  EXPECT_EQ(0, merged_event.modifiers);
  EXPECT_EQ(16, merged_event.data.scrollUpdate.deltaX);
  EXPECT_EQ(-11, merged_event.data.scrollUpdate.deltaY);

  // Enqueued.
  SimulateGestureScrollUpdateEvent(8, -7, 1);

  // Check that we didn't wrongly coalesce.
  merged_event = host_->GestureEventLastQueueEvent();
  EXPECT_EQ(WebInputEvent::GestureScrollUpdate, merged_event.type);
  EXPECT_EQ(1, merged_event.modifiers);

  // Different.
  SimulateGestureEvent(WebInputEvent::GestureScrollEnd);

  // Check that only the first event was sent.
  EXPECT_EQ(1U, process_->sink().message_count());
  EXPECT_TRUE(process_->sink().GetUniqueMessageMatching(
              ViewMsg_HandleInputEvent::ID));
  process_->sink().ClearMessages();

  // Check that the ACK sends the second message.
  SendInputEventACK(WebInputEvent::GestureScrollBegin, true);
  MessageLoop::current()->RunAllPending();
  EXPECT_EQ(1U, process_->sink().message_count());
  EXPECT_TRUE(process_->sink().GetUniqueMessageMatching(
              ViewMsg_HandleInputEvent::ID));
  process_->sink().ClearMessages();

  // Ack for queued coalesced event.
  SendInputEventACK(WebInputEvent::GestureScrollUpdate, true);
  MessageLoop::current()->RunAllPending();
  EXPECT_EQ(1U, process_->sink().message_count());
  EXPECT_TRUE(process_->sink().GetUniqueMessageMatching(
              ViewMsg_HandleInputEvent::ID));
  process_->sink().ClearMessages();

  // Ack for queued uncoalesced event.
  SendInputEventACK(WebInputEvent::GestureScrollUpdate, true);
  MessageLoop::current()->RunAllPending();
  EXPECT_EQ(1U, process_->sink().message_count());
  EXPECT_TRUE(process_->sink().GetUniqueMessageMatching(
              ViewMsg_HandleInputEvent::ID));
  process_->sink().ClearMessages();

  // After the final ack, the queue should be empty.
  SendInputEventACK(WebInputEvent::GestureScrollEnd, true);
  MessageLoop::current()->RunAllPending();
  EXPECT_EQ(0U, process_->sink().message_count());
}

TEST_F(RenderWidgetHostTest, GestureFlingCancelsFiltered) {
  // Turn off debounce handling for test isolation.
  host_->set_debounce_interval_time_ms(0);
  process_->sink().ClearMessages();
  // GFC without previous GFS is dropped.
  SimulateGestureEvent(WebInputEvent::GestureFlingCancel);
  EXPECT_EQ(0U, process_->sink().message_count());
  EXPECT_EQ(0U, host_->GestureEventLastQueueEventSize());

  // GFC after previous GFS is dispatched and acked.
  process_->sink().ClearMessages();
  SimulateGestureFlingStartEvent(0, -10);
  EXPECT_TRUE(host_->FlingInProgress());
  SendInputEventACK(WebInputEvent::GestureFlingStart, true);
  MessageLoop::current()->RunAllPending();
  SimulateGestureEvent(WebInputEvent::GestureFlingCancel);
  EXPECT_FALSE(host_->FlingInProgress());
  EXPECT_EQ(2U, process_->sink().message_count());
  SendInputEventACK(WebInputEvent::GestureFlingCancel, true);
  MessageLoop::current()->RunAllPending();
  EXPECT_EQ(0U, host_->GestureEventLastQueueEventSize());

  // GFC before previous GFS is acked.
  process_->sink().ClearMessages();
  SimulateGestureFlingStartEvent(0, -10);
  EXPECT_TRUE(host_->FlingInProgress());
  SimulateGestureEvent(WebInputEvent::GestureFlingCancel);
  EXPECT_FALSE(host_->FlingInProgress());
  EXPECT_EQ(1U, process_->sink().message_count());
  EXPECT_EQ(2U, host_->GestureEventLastQueueEventSize());

  // Advance state realistically.
  SendInputEventACK(WebInputEvent::GestureFlingStart, true);
  MessageLoop::current()->RunAllPending();
  SendInputEventACK(WebInputEvent::GestureFlingCancel, true);
  MessageLoop::current()->RunAllPending();
  EXPECT_EQ(0U, host_->GestureEventLastQueueEventSize());

  // GFS is added to the queue if another event is pending
  process_->sink().ClearMessages();
  SimulateGestureScrollUpdateEvent(8, -7, 0);
  SimulateGestureFlingStartEvent(0, -10);
  EXPECT_EQ(2U, host_->GestureEventLastQueueEventSize());
  EXPECT_EQ(1U, process_->sink().message_count());
  WebGestureEvent merged_event = host_->GestureEventLastQueueEvent();
  EXPECT_EQ(WebInputEvent::GestureFlingStart, merged_event.type);
  EXPECT_TRUE(host_->FlingInProgress());
  EXPECT_EQ(2U, host_->GestureEventLastQueueEventSize());

  // GFS in queue means that a GFC is added to the queue
  SimulateGestureEvent(WebInputEvent::GestureFlingCancel);
  merged_event =host_->GestureEventLastQueueEvent();
  EXPECT_EQ(WebInputEvent::GestureFlingCancel, merged_event.type);
  EXPECT_FALSE(host_->FlingInProgress());
  EXPECT_EQ(3U, host_->GestureEventLastQueueEventSize());

  // Adding a second GFC is dropped.
  SimulateGestureEvent(WebInputEvent::GestureFlingCancel);
  EXPECT_FALSE(host_->FlingInProgress());
  EXPECT_EQ(3U, host_->GestureEventLastQueueEventSize());

  // Adding another GFS will add it to the queue.
  SimulateGestureFlingStartEvent(0, -10);
  merged_event = host_->GestureEventLastQueueEvent();
  EXPECT_EQ(WebInputEvent::GestureFlingStart, merged_event.type);
  EXPECT_TRUE(host_->FlingInProgress());
  EXPECT_EQ(4U, host_->GestureEventLastQueueEventSize());

  // GFS in queue means that a GFC is added to the queue
  SimulateGestureEvent(WebInputEvent::GestureFlingCancel);
  merged_event = host_->GestureEventLastQueueEvent();
  EXPECT_EQ(WebInputEvent::GestureFlingCancel, merged_event.type);
  EXPECT_FALSE(host_->FlingInProgress());
  EXPECT_EQ(5U, host_->GestureEventLastQueueEventSize());

  // Adding another GFC with a GFC already there is dropped.
  SimulateGestureEvent(WebInputEvent::GestureFlingCancel);
  merged_event = host_->GestureEventLastQueueEvent();
  EXPECT_EQ(WebInputEvent::GestureFlingCancel, merged_event.type);
  EXPECT_FALSE(host_->FlingInProgress());
  EXPECT_EQ(5U, host_->GestureEventLastQueueEventSize());
}

// Test that GestureTapDown events are deferred.
TEST_F(RenderWidgetHostTest, DeferredGestureTapDown) {
  process_->sink().ClearMessages();

  // Set some sort of short deferral timeout
  host_->set_maximum_tap_gap_time_ms(5);

  SimulateGestureEvent(WebInputEvent::GestureTapDown);
  EXPECT_EQ(0U, process_->sink().message_count());
  EXPECT_EQ(0U, host_->GestureEventLastQueueEventSize());

  // Wait long enough for first timeout and see if it fired.
  MessageLoop::current()->PostDelayedTask(
      FROM_HERE, MessageLoop::QuitClosure(), TimeDelta::FromMilliseconds(10));
  MessageLoop::current()->Run();

  EXPECT_EQ(1U, process_->sink().message_count());
  EXPECT_EQ(1U, host_->GestureEventLastQueueEventSize());
  EXPECT_EQ(WebInputEvent::GestureTapDown,
            host_->GestureEventLastQueueEvent().type);
}

// Test that GestureTapDown events are sent immediately on GestureTap.
TEST_F(RenderWidgetHostTest, DeferredGestureTapDownSentOnTap) {
  process_->sink().ClearMessages();

  // Set some sort of short deferral timeout
  host_->set_maximum_tap_gap_time_ms(5);

  SimulateGestureEvent(WebInputEvent::GestureTapDown);
  EXPECT_EQ(0U, process_->sink().message_count());
  EXPECT_EQ(0U, host_->GestureEventLastQueueEventSize());

  SimulateGestureEvent(WebInputEvent::GestureTap);
  EXPECT_EQ(1U, process_->sink().message_count());
  EXPECT_EQ(2U, host_->GestureEventLastQueueEventSize());
  EXPECT_EQ(WebInputEvent::GestureTap,
            host_->GestureEventLastQueueEvent().type);

  MessageLoop::current()->PostDelayedTask(
      FROM_HERE, MessageLoop::QuitClosure(), TimeDelta::FromMilliseconds(10));
  MessageLoop::current()->Run();

  // If the deferral timer incorrectly fired, it sent an extra message.
  EXPECT_EQ(1U, process_->sink().message_count());
}

// Test that only a single GestureTapDown event is sent when tap occurs after
// the timeout.
TEST_F(RenderWidgetHostTest, DeferredGestureTapDownOnlyOnce) {
  process_->sink().ClearMessages();

  // Set some sort of short deferral timeout
  host_->set_maximum_tap_gap_time_ms(5);

  SimulateGestureEvent(WebInputEvent::GestureTapDown);
  EXPECT_EQ(0U, process_->sink().message_count());
  EXPECT_EQ(0U, host_->GestureEventLastQueueEventSize());

  // Wait long enough for the timeout and verify it fired.
  MessageLoop::current()->PostDelayedTask(
      FROM_HERE, MessageLoop::QuitClosure(), TimeDelta::FromMilliseconds(10));
  MessageLoop::current()->Run();

  EXPECT_EQ(1U, process_->sink().message_count());
  EXPECT_EQ(1U, host_->GestureEventLastQueueEventSize());
  EXPECT_EQ(WebInputEvent::GestureTapDown,
            host_->GestureEventLastQueueEvent().type);

  // Now send the tap gesture and verify we didn't get an extra TapDown.
  SimulateGestureEvent(WebInputEvent::GestureTap);
  EXPECT_EQ(1U, process_->sink().message_count());
  EXPECT_EQ(2U, host_->GestureEventLastQueueEventSize());
  EXPECT_EQ(WebInputEvent::GestureTap,
            host_->GestureEventLastQueueEvent().type);
}

// Test that scroll events during the deferral interval drop the GestureTapDown.
TEST_F(RenderWidgetHostTest, DeferredGestureTapDownAnulledOnScroll) {
  process_->sink().ClearMessages();

  // Set some sort of short deferral timeout
  host_->set_maximum_tap_gap_time_ms(5);

  SimulateGestureEvent(WebInputEvent::GestureTapDown);
  EXPECT_EQ(0U, process_->sink().message_count());
  EXPECT_EQ(0U, host_->GestureEventLastQueueEventSize());

  SimulateGestureEvent(WebInputEvent::GestureScrollBegin);
  EXPECT_EQ(1U, process_->sink().message_count());
  EXPECT_EQ(1U, host_->GestureEventLastQueueEventSize());
  EXPECT_EQ(WebInputEvent::GestureScrollBegin,
            host_->GestureEventLastQueueEvent().type);

  MessageLoop::current()->PostDelayedTask(
      FROM_HERE, MessageLoop::QuitClosure(), TimeDelta::FromMilliseconds(10));
  MessageLoop::current()->Run();

  // If the deferral timer incorrectly fired, it will send an extra message.
  EXPECT_EQ(1U, process_->sink().message_count());
}

// Test that a tap cancel event during the deferral interval drops the
// GestureTapDown.
TEST_F(RenderWidgetHostTest, DeferredGestureTapDownAnulledOnTapCancel) {
  process_->sink().ClearMessages();

  // Set some sort of short deferral timeout
  host_->set_maximum_tap_gap_time_ms(5);

  SimulateGestureEvent(WebInputEvent::GestureTapDown);
  EXPECT_EQ(0U, process_->sink().message_count());
  EXPECT_EQ(0U, host_->GestureEventLastQueueEventSize());

  SimulateGestureEvent(WebInputEvent::GestureTapCancel);
  EXPECT_EQ(0U, process_->sink().message_count());
  EXPECT_EQ(0U, host_->GestureEventLastQueueEventSize());

  MessageLoop::current()->PostDelayedTask(
      FROM_HERE, MessageLoop::QuitClosure(), TimeDelta::FromMilliseconds(10));
  MessageLoop::current()->Run();

  // If the deferral timer incorrectly fired, it will send an extra message.
  EXPECT_EQ(0U, process_->sink().message_count());
}

// Test that if a GestureTapDown gets sent, any corresponding GestureTapCancel
// is also sent.
TEST_F(RenderWidgetHostTest, DeferredGestureTapDownTapCancel) {
  process_->sink().ClearMessages();

  // Set some sort of short deferral timeout
  host_->set_maximum_tap_gap_time_ms(5);

  SimulateGestureEvent(WebInputEvent::GestureTapDown);
  EXPECT_EQ(0U, process_->sink().message_count());
  EXPECT_EQ(0U, host_->GestureEventLastQueueEventSize());

  MessageLoop::current()->PostDelayedTask(
      FROM_HERE, MessageLoop::QuitClosure(), TimeDelta::FromMilliseconds(10));
  MessageLoop::current()->Run();

  EXPECT_EQ(1U, process_->sink().message_count());
  EXPECT_EQ(1U, host_->GestureEventLastQueueEventSize());

  SimulateGestureEvent(WebInputEvent::GestureTapCancel);
  EXPECT_EQ(1U, process_->sink().message_count());
  EXPECT_EQ(2U, host_->GestureEventLastQueueEventSize());
}

// Test that a GestureScrollEnd | GestureFlingStart are deferred during the
// debounce interval, that Scrolls are not and that the deferred events are
// sent after that timer fires.
TEST_F(RenderWidgetHostTest, DebounceDefersFollowingGestureEvents) {
  process_->sink().ClearMessages();

  host_->set_debounce_interval_time_ms(3);

  SimulateGestureEvent(WebInputEvent::GestureScrollUpdate);
  EXPECT_EQ(1U, process_->sink().message_count());
  EXPECT_EQ(1U, host_->GestureEventLastQueueEventSize());
  EXPECT_EQ(0U, host_->GestureEventDebouncingQueueSize());
  EXPECT_TRUE(host_->ScrollingInProgress());

  SimulateGestureEvent(WebInputEvent::GestureScrollUpdate);
  EXPECT_EQ(1U, process_->sink().message_count());
  EXPECT_EQ(2U, host_->GestureEventLastQueueEventSize());
  EXPECT_EQ(0U, host_->GestureEventDebouncingQueueSize());
  EXPECT_TRUE(host_->ScrollingInProgress());

  SimulateGestureEvent(WebInputEvent::GestureScrollEnd);
  EXPECT_EQ(1U, process_->sink().message_count());
  EXPECT_EQ(2U, host_->GestureEventLastQueueEventSize());
  EXPECT_EQ(1U, host_->GestureEventDebouncingQueueSize());

  SimulateGestureEvent(WebInputEvent::GestureFlingStart);
  EXPECT_EQ(1U, process_->sink().message_count());
  EXPECT_EQ(2U, host_->GestureEventLastQueueEventSize());
  EXPECT_EQ(2U, host_->GestureEventDebouncingQueueSize());

  SimulateGestureEvent(WebInputEvent::GestureTapDown);
  EXPECT_EQ(1U, process_->sink().message_count());
  EXPECT_EQ(2U, host_->GestureEventLastQueueEventSize());
  EXPECT_EQ(3U, host_->GestureEventDebouncingQueueSize());

  MessageLoop::current()->PostDelayedTask(
      FROM_HERE, MessageLoop::QuitClosure(), TimeDelta::FromMilliseconds(5));
  MessageLoop::current()->Run();

  // The deferred events are correctly queued in coalescing queue.
  EXPECT_EQ(1U, process_->sink().message_count());
  // NOTE: The  TapDown is still deferred hence not queued.
  EXPECT_EQ(4U, host_->GestureEventLastQueueEventSize());
  EXPECT_EQ(0U, host_->GestureEventDebouncingQueueSize());
  EXPECT_FALSE(host_->ScrollingInProgress());

  // Verify that the coalescing queue contains the correct events.
  WebInputEvent::Type expected[] = {
      WebInputEvent::GestureScrollUpdate,
      WebInputEvent::GestureScrollUpdate,
      WebInputEvent::GestureScrollEnd,
      WebInputEvent::GestureFlingStart};

  for (unsigned i = 0; i < sizeof(expected) / sizeof(WebInputEvent::Type);
      i++) {
    WebGestureEvent merged_event = host_->GestureEventQueueEventAt(i);
    EXPECT_EQ(expected[i], merged_event.type);
  }
}

// Test that non-scroll events are deferred while scrolling during the debounce
// interval and are discarded if a GestureScrollUpdate event arrives before the
// interval end.
TEST_F(RenderWidgetHostTest, DebounceDropsDeferredEvents) {
  process_->sink().ClearMessages();

  host_->set_debounce_interval_time_ms(3);
  EXPECT_FALSE(host_->ScrollingInProgress());

  SimulateGestureEvent(WebInputEvent::GestureScrollUpdate);
  EXPECT_EQ(1U, process_->sink().message_count());
  EXPECT_EQ(1U, host_->GestureEventLastQueueEventSize());
  EXPECT_EQ(0U, host_->GestureEventDebouncingQueueSize());
  EXPECT_TRUE(host_->ScrollingInProgress());

  // This event should get discarded.
  SimulateGestureEvent(WebInputEvent::GestureScrollEnd);
  EXPECT_EQ(1U, process_->sink().message_count());
  EXPECT_EQ(1U, host_->GestureEventLastQueueEventSize());
  EXPECT_EQ(1U, host_->GestureEventDebouncingQueueSize());

  SimulateGestureEvent(WebInputEvent::GestureScrollUpdate);
  EXPECT_EQ(1U, process_->sink().message_count());
  EXPECT_EQ(2U, host_->GestureEventLastQueueEventSize());
  EXPECT_EQ(0U, host_->GestureEventDebouncingQueueSize());
  EXPECT_TRUE(host_->ScrollingInProgress());

  // Verify that the coalescing queue contains the correct events.
  WebInputEvent::Type expected[] = {
      WebInputEvent::GestureScrollUpdate,
      WebInputEvent::GestureScrollUpdate};

  for (unsigned i = 0; i < sizeof(expected) / sizeof(WebInputEvent::Type);
      i++) {
    WebGestureEvent merged_event = host_->GestureEventQueueEventAt(i);
    EXPECT_EQ(expected[i], merged_event.type);
  }
}

// Tests that touch-events are queued properly.
TEST_F(RenderWidgetHostTest, TouchEventQueue) {
  process_->sink().ClearMessages();

  PressTouchPoint(1, 1);
  SendTouchEvent();
  EXPECT_EQ(1U, process_->sink().message_count());
  process_->sink().ClearMessages();

  // The second touch should not be sent since one is already in queue.
  MoveTouchPoint(0, 5, 5);
  SendTouchEvent();
  EXPECT_EQ(0U, process_->sink().message_count());

  EXPECT_EQ(2U, host_->TouchEventQueueSize());

  // Receive an ACK for the first touch-event.
  SendInputEventACK(WebInputEvent::TouchStart, true);
  EXPECT_EQ(1U, host_->TouchEventQueueSize());
  EXPECT_EQ(WebKit::WebInputEvent::TouchStart, view_->acked_event().type);
  EXPECT_EQ(1, view_->acked_event_count());
  EXPECT_EQ(1U, process_->sink().message_count());
  process_->sink().ClearMessages();
  view_->ClearAckedEvent();

  SendInputEventACK(WebInputEvent::TouchMove, true);
  EXPECT_EQ(0U, host_->TouchEventQueueSize());
  EXPECT_EQ(WebKit::WebInputEvent::TouchMove, view_->acked_event().type);
  EXPECT_EQ(1, view_->acked_event_count());
  EXPECT_EQ(0U, process_->sink().message_count());
}

// Tests that the touch-queue is emptied if a page stops listening for touch
// events.
TEST_F(RenderWidgetHostTest, TouchEventQueueFlush) {
  process_->sink().ClearMessages();

  host_->OnMessageReceived(ViewHostMsg_HasTouchEventHandlers(0, true));
  EXPECT_EQ(0U, process_->sink().message_count());
  EXPECT_EQ(0U, host_->TouchEventQueueSize());

  EXPECT_EQ(0U, host_->TouchEventQueueSize());
  EXPECT_TRUE(host_->ShouldForwardTouchEvent());

  // Send a touch-press event.
  PressTouchPoint(1, 1);
  SendTouchEvent();
  EXPECT_EQ(1U, process_->sink().message_count());
  process_->sink().ClearMessages();

  ReleaseTouchPoint(0);
  SendTouchEvent();

  for (int i = 5; i < 15; ++i) {
    PressTouchPoint(1, 1);
    SendTouchEvent();
    MoveTouchPoint(0, i, i);
    SendTouchEvent();
    ReleaseTouchPoint(0);
    SendTouchEvent();
  }
  EXPECT_EQ(0U, process_->sink().message_count());
  EXPECT_EQ(32U, host_->TouchEventQueueSize());

  // Receive an ACK for the first touch-event. One of the queued touch-event
  // should be forwarded.
  SendInputEventACK(WebInputEvent::TouchStart, true);
  EXPECT_EQ(31U, host_->TouchEventQueueSize());
  EXPECT_EQ(WebKit::WebInputEvent::TouchStart, view_->acked_event().type);
  EXPECT_EQ(1, view_->acked_event_count());
  EXPECT_EQ(1U, process_->sink().message_count());
  process_->sink().ClearMessages();
  view_->ClearAckedEvent();

  // The page stops listening for touch-events. The touch-event queue should now
  // be emptied, but none of the queued touch-events should be sent to the
  // renderer.
  host_->OnMessageReceived(ViewHostMsg_HasTouchEventHandlers(0, false));
  EXPECT_EQ(0U, process_->sink().message_count());
  EXPECT_EQ(0U, host_->TouchEventQueueSize());
  EXPECT_FALSE(host_->ShouldForwardTouchEvent());
}

// Tests that touch-events are coalesced properly in the queue.
TEST_F(RenderWidgetHostTest, TouchEventQueueCoalesce) {
  process_->sink().ClearMessages();

  host_->OnMessageReceived(ViewHostMsg_HasTouchEventHandlers(0, true));
  EXPECT_EQ(0U, process_->sink().message_count());
  EXPECT_EQ(0U, host_->TouchEventQueueSize());
  EXPECT_TRUE(host_->ShouldForwardTouchEvent());

  // Send a touch-press event.
  PressTouchPoint(1, 1);
  SendTouchEvent();
  EXPECT_EQ(1U, process_->sink().message_count());
  process_->sink().ClearMessages();

  // Send a few touch-move events, followed by a touch-release event. All the
  // touch-move events should be coalesced into a single event.
  for (int i = 5; i < 15; ++i) {
    MoveTouchPoint(0, i, i);
    SendTouchEvent();
  }
  ReleaseTouchPoint(0);
  SendTouchEvent();
  EXPECT_EQ(0U, process_->sink().message_count());
  EXPECT_EQ(3U, host_->TouchEventQueueSize());

  // ACK the press.
  SendInputEventACK(WebInputEvent::TouchStart, true);
  EXPECT_EQ(1U, process_->sink().message_count());
  EXPECT_EQ(2U, host_->TouchEventQueueSize());
  EXPECT_EQ(WebKit::WebInputEvent::TouchStart, view_->acked_event().type);
  EXPECT_EQ(1, view_->acked_event_count());
  process_->sink().ClearMessages();
  view_->ClearAckedEvent();

  // ACK the moves.
  SendInputEventACK(WebInputEvent::TouchMove, true);
  EXPECT_EQ(1U, process_->sink().message_count());
  EXPECT_EQ(1U, host_->TouchEventQueueSize());
  EXPECT_EQ(WebKit::WebInputEvent::TouchMove, view_->acked_event().type);
  EXPECT_EQ(10, view_->acked_event_count());
  process_->sink().ClearMessages();
  view_->ClearAckedEvent();

  // ACK the release.
  SendInputEventACK(WebInputEvent::TouchEnd, true);
  EXPECT_EQ(0U, process_->sink().message_count());
  EXPECT_EQ(0U, host_->TouchEventQueueSize());
  EXPECT_EQ(WebKit::WebInputEvent::TouchEnd, view_->acked_event().type);
  EXPECT_EQ(1, view_->acked_event_count());
}

// Tests that an event that has already been sent but hasn't been ack'ed yet
// doesn't get coalesced with newer events.
TEST_F(RenderWidgetHostTest, SentTouchEventDoesNotCoalesce) {
  process_->sink().ClearMessages();

  host_->OnMessageReceived(ViewHostMsg_HasTouchEventHandlers(0, true));
  EXPECT_EQ(0U, process_->sink().message_count());
  EXPECT_EQ(0U, host_->TouchEventQueueSize());
  EXPECT_TRUE(host_->ShouldForwardTouchEvent());

  // Send a touch-press event.
  PressTouchPoint(1, 1);
  SendTouchEvent();
  EXPECT_EQ(1U, process_->sink().message_count());
  process_->sink().ClearMessages();

  // Send a few touch-move events, followed by a touch-release event. All the
  // touch-move events should be coalesced into a single event.
  for (int i = 5; i < 15; ++i) {
    MoveTouchPoint(0, i, i);
    SendTouchEvent();
  }
  EXPECT_EQ(0U, process_->sink().message_count());
  EXPECT_EQ(2U, host_->TouchEventQueueSize());

  SendInputEventACK(WebInputEvent::TouchStart, false);
  EXPECT_EQ(1U, process_->sink().message_count());
  EXPECT_EQ(1U, host_->TouchEventQueueSize());
  process_->sink().ClearMessages();

  // The coalesced touch-move event has been sent to the renderer. Any new
  // touch-move event should not be coalesced with the sent event.
  MoveTouchPoint(0, 5, 5);
  SendTouchEvent();
  EXPECT_EQ(2U, host_->TouchEventQueueSize());

  MoveTouchPoint(0, 7, 7);
  SendTouchEvent();
  EXPECT_EQ(2U, host_->TouchEventQueueSize());
}

// Tests that coalescing works correctly for multi-touch events.
TEST_F(RenderWidgetHostTest, TouchEventQueueMultiTouch) {
  process_->sink().ClearMessages();

  host_->OnMessageReceived(ViewHostMsg_HasTouchEventHandlers(0, true));
  EXPECT_EQ(0U, process_->sink().message_count());
  EXPECT_EQ(0U, host_->TouchEventQueueSize());
  EXPECT_TRUE(host_->ShouldForwardTouchEvent());

  // Press the first finger.
  PressTouchPoint(1, 1);
  SendTouchEvent();
  EXPECT_EQ(1U, process_->sink().message_count());
  process_->sink().ClearMessages();

  // Move the finger.
  MoveTouchPoint(0, 5, 5);
  SendTouchEvent();
  EXPECT_EQ(2U, host_->TouchEventQueueSize());

  // Now press a second finger.
  PressTouchPoint(2, 2);
  SendTouchEvent();
  EXPECT_EQ(3U, host_->TouchEventQueueSize());

  // Move both fingers.
  MoveTouchPoint(0, 10, 10);
  MoveTouchPoint(1, 20, 20);
  SendTouchEvent();
  EXPECT_EQ(4U, host_->TouchEventQueueSize());

  // Move only one finger now.
  MoveTouchPoint(0, 15, 15);
  SendTouchEvent();
  EXPECT_EQ(4U, host_->TouchEventQueueSize());

  // Move the other finger.
  MoveTouchPoint(1, 25, 25);
  SendTouchEvent();
  EXPECT_EQ(4U, host_->TouchEventQueueSize());

  // Make sure both fingers are marked as having been moved in the coalesced
  // event.
  const WebTouchEvent& event = host_->latest_event();
  EXPECT_EQ(WebTouchPoint::StateMoved, event.touches[0].state);
  EXPECT_EQ(WebTouchPoint::StateMoved, event.touches[1].state);
}

// Tests that if a touch-event queue is destroyed in response to a touch-event
// in the renderer, then there is no crash when the ACK for that touch-event
// comes back.
TEST_F(RenderWidgetHostTest, TouchEventAckAfterQueueFlushed) {
  // First, install a touch-event handler and send some touch-events to the
  // renderer.
  process_->sink().ClearMessages();
  host_->OnMessageReceived(ViewHostMsg_HasTouchEventHandlers(0, true));
  EXPECT_EQ(0U, process_->sink().message_count());
  EXPECT_EQ(0U, host_->TouchEventQueueSize());
  EXPECT_TRUE(host_->ShouldForwardTouchEvent());

  PressTouchPoint(1, 1);
  SendTouchEvent();
  EXPECT_EQ(1U, process_->sink().message_count());
  EXPECT_EQ(1U, host_->TouchEventQueueSize());
  process_->sink().ClearMessages();

  MoveTouchPoint(0, 10, 10);
  SendTouchEvent();
  EXPECT_EQ(0U, process_->sink().message_count());
  EXPECT_EQ(2U, host_->TouchEventQueueSize());

  // Receive an ACK for the press. This should cause the queued touch-move to
  // be sent to the renderer.
  SendInputEventACK(WebInputEvent::TouchStart, true);
  EXPECT_EQ(1U, process_->sink().message_count());
  EXPECT_EQ(1U, host_->TouchEventQueueSize());
  process_->sink().ClearMessages();

  // Uninstall the touch-event handler. This will cause the queue to be flushed.
  host_->OnMessageReceived(ViewHostMsg_HasTouchEventHandlers(0, false));
  EXPECT_EQ(0U, process_->sink().message_count());
  EXPECT_EQ(0U, host_->TouchEventQueueSize());

  // Now receive an ACK for the move.
  SendInputEventACK(WebInputEvent::TouchMove, true);
  EXPECT_EQ(0U, process_->sink().message_count());
  EXPECT_EQ(0U, host_->TouchEventQueueSize());
}

#if defined(OS_WIN) || defined(USE_AURA)
// Tests that the acked events have correct state. (ui::Events are used only on
// windows and aura)
TEST_F(RenderWidgetHostTest, AckedTouchEventState) {
  process_->sink().ClearMessages();
  host_->OnMessageReceived(ViewHostMsg_HasTouchEventHandlers(0, true));
  EXPECT_EQ(0U, process_->sink().message_count());
  EXPECT_EQ(0U, host_->TouchEventQueueSize());
  EXPECT_TRUE(host_->ShouldForwardTouchEvent());

  // Send a bunch of events, and make sure the ACKed events are correct.
  ScopedVector<ui::TouchEvent> expected_events;

  // Use a custom timestamp for all the events to test that the acked events
  // have the same timestamp;
  base::TimeDelta timestamp = base::Time::NowFromSystemTime() - base::Time();
  timestamp -= base::TimeDelta::FromSeconds(600);

  // Press the first finger.
  PressTouchPoint(1, 1);
  SetTouchTimestamp(timestamp);
  SendTouchEvent();
  EXPECT_EQ(1U, process_->sink().message_count());
  process_->sink().ClearMessages();
  expected_events.push_back(new ui::TouchEvent(ui::ET_TOUCH_PRESSED,
      gfx::Point(1, 1), 0, timestamp));

  // Move the finger.
  timestamp += base::TimeDelta::FromSeconds(10);
  MoveTouchPoint(0, 5, 5);
  SetTouchTimestamp(timestamp);
  SendTouchEvent();
  EXPECT_EQ(2U, host_->TouchEventQueueSize());
  expected_events.push_back(new ui::TouchEvent(ui::ET_TOUCH_MOVED,
      gfx::Point(5, 5), 0, timestamp));

  // Now press a second finger.
  timestamp += base::TimeDelta::FromSeconds(10);
  PressTouchPoint(2, 2);
  SetTouchTimestamp(timestamp);
  SendTouchEvent();
  EXPECT_EQ(3U, host_->TouchEventQueueSize());
  expected_events.push_back(new ui::TouchEvent(ui::ET_TOUCH_PRESSED,
      gfx::Point(2, 2), 1, timestamp));

  // Move both fingers.
  timestamp += base::TimeDelta::FromSeconds(10);
  MoveTouchPoint(0, 10, 10);
  MoveTouchPoint(1, 20, 20);
  SetTouchTimestamp(timestamp);
  SendTouchEvent();
  EXPECT_EQ(4U, host_->TouchEventQueueSize());
  expected_events.push_back(new ui::TouchEvent(ui::ET_TOUCH_MOVED,
      gfx::Point(10, 10), 0, timestamp));
  expected_events.push_back(new ui::TouchEvent(ui::ET_TOUCH_MOVED,
      gfx::Point(20, 20), 1, timestamp));

  // Receive the ACKs and make sure the generated events from the acked events
  // are correct.
  WebInputEvent::Type acks[] = { WebInputEvent::TouchStart,
                                 WebInputEvent::TouchMove,
                                 WebInputEvent::TouchStart,
                                 WebInputEvent::TouchMove };

  for (size_t i = 0; i < arraysize(acks); ++i) {
    SendInputEventACK(acks[i], false);
    EXPECT_EQ(acks[i], view_->acked_event().type);
    ScopedVector<ui::TouchEvent> acked;

    MakeUITouchEventsFromWebTouchEvents(view_->acked_event(), &acked);
    bool success = EventListIsSubset(acked, expected_events);
    EXPECT_TRUE(success) << "Failed on step: " << i;
    if (!success)
      break;
    expected_events.erase(expected_events.begin(),
                          expected_events.begin() + acked.size());
  }

  EXPECT_EQ(0U, expected_events.size());
}
#endif  // defined(OS_WIN) || defined(USE_AURA)

// Test that the hang monitor timer expires properly if a new timer is started
// while one is in progress (see crbug.com/11007).
TEST_F(RenderWidgetHostTest, DontPostponeHangMonitorTimeout) {
  // Start with a short timeout.
  host_->StartHangMonitorTimeout(TimeDelta::FromMilliseconds(10));

  // Immediately try to add a long 30 second timeout.
  EXPECT_FALSE(host_->unresponsive_timer_fired());
  host_->StartHangMonitorTimeout(TimeDelta::FromSeconds(30));

  // Wait long enough for first timeout and see if it fired.
  MessageLoop::current()->PostDelayedTask(
      FROM_HERE, MessageLoop::QuitClosure(), TimeDelta::FromMilliseconds(10));
  MessageLoop::current()->Run();
  EXPECT_TRUE(host_->unresponsive_timer_fired());
}

// Test that the hang monitor timer expires properly if it is started, stopped,
// and then started again.
TEST_F(RenderWidgetHostTest, StopAndStartHangMonitorTimeout) {
  // Start with a short timeout, then stop it.
  host_->StartHangMonitorTimeout(TimeDelta::FromMilliseconds(10));
  host_->StopHangMonitorTimeout();

  // Start it again to ensure it still works.
  EXPECT_FALSE(host_->unresponsive_timer_fired());
  host_->StartHangMonitorTimeout(TimeDelta::FromMilliseconds(10));

  // Wait long enough for first timeout and see if it fired.
  MessageLoop::current()->PostDelayedTask(
      FROM_HERE, MessageLoop::QuitClosure(), TimeDelta::FromMilliseconds(40));
  MessageLoop::current()->Run();
  EXPECT_TRUE(host_->unresponsive_timer_fired());
}

// Test that the hang monitor timer expires properly if it is started, then
// updated to a shorter duration.
TEST_F(RenderWidgetHostTest, ShorterDelayHangMonitorTimeout) {
  // Start with a timeout.
  host_->StartHangMonitorTimeout(TimeDelta::FromMilliseconds(100));

  // Start it again with shorter delay.
  EXPECT_FALSE(host_->unresponsive_timer_fired());
  host_->StartHangMonitorTimeout(TimeDelta::FromMilliseconds(20));

  // Wait long enough for the second timeout and see if it fired.
  MessageLoop::current()->PostDelayedTask(
      FROM_HERE, MessageLoop::QuitClosure(), TimeDelta::FromMilliseconds(25));
  MessageLoop::current()->Run();
  EXPECT_TRUE(host_->unresponsive_timer_fired());
}

// Test that the hang monitor catches two input events but only one ack.
// This can happen if the second input event causes the renderer to hang.
// This test will catch a regression of crbug.com/111185.
TEST_F(RenderWidgetHostTest, MultipleInputEvents) {
  // Configure the host to wait 10ms before considering
  // the renderer hung.
  host_->set_hung_renderer_delay_ms(10);

  // Send two events but only one ack.
  SimulateKeyboardEvent(WebInputEvent::RawKeyDown);
  SimulateKeyboardEvent(WebInputEvent::RawKeyDown);
  SendInputEventACK(WebInputEvent::RawKeyDown, true);

  // Wait long enough for first timeout and see if it fired.
  MessageLoop::current()->PostDelayedTask(
      FROM_HERE, MessageLoop::QuitClosure(), TimeDelta::FromMilliseconds(40));
  MessageLoop::current()->Run();
  EXPECT_TRUE(host_->unresponsive_timer_fired());
}

// This test is not valid for Windows because getting the shared memory
// size doesn't work.
#if !defined(OS_WIN)
TEST_F(RenderWidgetHostTest, IncorrectBitmapScaleFactor) {
  ViewHostMsg_UpdateRect_Params params;
  process_->InitUpdateRectParams(&params);
  params.scale_factor = params.scale_factor * 2;

  EXPECT_EQ(0, process_->bad_msg_count());
  host_->OnMsgUpdateRect(params);
  EXPECT_EQ(1, process_->bad_msg_count());
}
#endif

}  // namespace content
