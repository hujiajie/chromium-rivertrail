// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/cocoa/constrained_window/constrained_window_mac2.h"

#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/cocoa/browser_window_controller.h"
#include "chrome/browser/ui/tab_contents/tab_contents.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/browser/web_contents.h"
#include "googleurl/src/gurl.h"
#include "testing/gmock/include/gmock/gmock.h"

using ::testing::NiceMock;

namespace {

class ConstrainedWindowDelegateMock : public ConstrainedWindowMacDelegate2 {
 public:
  MOCK_METHOD1(OnConstrainedWindowClosed, void(ConstrainedWindowMac2*));
};

}  // namespace

class ConstrainedWindowMacTest : public InProcessBrowserTest {
 public:
  ConstrainedWindowMacTest()
      : InProcessBrowserTest(),
        tab0_(NULL),
        tab1_(NULL),
        controller_(NULL),
        tab_view0_(NULL),
        tab_view1_(NULL) {
    sheet_.reset([[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 30, 30)
                  styleMask:NSTitledWindowMask
                    backing:NSBackingStoreBuffered
                      defer:NO]);
    [sheet_ setReleasedWhenClosed:NO];
  }

  virtual void SetUpOnMainThread() OVERRIDE {
    AddTabAtIndex(1, GURL("about:blank"), content::PAGE_TRANSITION_LINK);
    tab0_ = browser()->tab_strip_model()->GetWebContentsAt(0);
    tab1_ = browser()->tab_strip_model()->GetWebContentsAt(1);
    EXPECT_EQ(tab1_, browser()->tab_strip_model()->GetActiveWebContents());

    controller_ = [BrowserWindowController browserWindowControllerForWindow:
        browser()->window()->GetNativeWindow()];
    EXPECT_TRUE(controller_);
    tab_view0_ = [[controller_ tabStripController] viewAtIndex:0];
    EXPECT_TRUE(tab_view0_);
    tab_view1_ = [[controller_ tabStripController] viewAtIndex:1];
    EXPECT_TRUE(tab_view1_);
  }

 protected:
  scoped_nsobject<NSWindow> sheet_;
  content::WebContents* tab0_;
  content::WebContents* tab1_;
  BrowserWindowController* controller_;
  NSView* tab_view0_;
  NSView* tab_view1_;
};

// Test that a sheet added to a inactive tab is not shown until the
// tab is activated.
IN_PROC_BROWSER_TEST_F(ConstrainedWindowMacTest, ShowInInactiveTab) {
  // Show dialog in non active tab.
  NiceMock<ConstrainedWindowDelegateMock> delegate;
  ConstrainedWindowMac2 dialog(&delegate, tab0_, sheet_);
  EXPECT_EQ(0.0, [sheet_ alphaValue]);

  // Switch to inactive tab.
  browser()->tab_strip_model()->ActivateTabAt(0, true);
  EXPECT_EQ(1.0, [sheet_ alphaValue]);

  dialog.CloseConstrainedWindow();
}

// Test that adding a sheet disables tab dragging.
IN_PROC_BROWSER_TEST_F(ConstrainedWindowMacTest, TabDragging) {
  NiceMock<ConstrainedWindowDelegateMock> delegate;
  ConstrainedWindowMac2 dialog(&delegate, tab1_, sheet_);

  // Verify that the dialog disables dragging.
  EXPECT_TRUE([controller_ isTabDraggable:tab_view0_]);
  EXPECT_FALSE([controller_ isTabDraggable:tab_view1_]);

  dialog.CloseConstrainedWindow();
}

// Test that closing a browser window with a sheet works.
IN_PROC_BROWSER_TEST_F(ConstrainedWindowMacTest, BrowserWindowClose) {
  NiceMock<ConstrainedWindowDelegateMock> delegate;
  ConstrainedWindowMac2 dialog(&delegate, tab1_, sheet_);
  EXPECT_EQ(1.0, [sheet_ alphaValue]);

  // Close the browser window.
  scoped_nsobject<NSWindow> browser_window(
      [browser()->window()->GetNativeWindow() retain]);
  EXPECT_TRUE([browser_window isVisible]);
  [browser()->window()->GetNativeWindow() performClose:nil];
  EXPECT_FALSE([browser_window isVisible]);
}

// Test that closing a tab with a sheet works.
IN_PROC_BROWSER_TEST_F(ConstrainedWindowMacTest, TabClose) {
  NiceMock<ConstrainedWindowDelegateMock> delegate;
  ConstrainedWindowMac2 dialog(&delegate, tab1_, sheet_);
  EXPECT_EQ(1.0, [sheet_ alphaValue]);

  // Close the tab.
  EXPECT_EQ(2, browser()->tab_count());
  EXPECT_TRUE(browser()->tab_strip_model()->CloseTabContentsAt(
      1, TabStripModel::CLOSE_USER_GESTURE));
  EXPECT_EQ(1, browser()->tab_count());
}

// Test that adding a sheet disables fullscreen.
IN_PROC_BROWSER_TEST_F(ConstrainedWindowMacTest, Fullscreen) {
  EXPECT_TRUE(chrome::IsCommandEnabled(browser(), IDC_FULLSCREEN));

  // Dialog will delete it self when closed.
  NiceMock<ConstrainedWindowDelegateMock> delegate;
  ConstrainedWindowMac2 dialog(&delegate, tab1_, sheet_);

  EXPECT_FALSE(chrome::IsCommandEnabled(browser(), IDC_FULLSCREEN));

  dialog.CloseConstrainedWindow();
}
