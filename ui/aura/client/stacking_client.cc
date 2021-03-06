// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/aura/client/stacking_client.h"

#include "ui/aura/env.h"
#include "ui/aura/root_window.h"
#include "ui/aura/window_property.h"

DECLARE_WINDOW_PROPERTY_TYPE(aura::client::StackingClient*)

namespace aura {
namespace client {

DEFINE_WINDOW_PROPERTY_KEY(
    StackingClient*, kRootWindowStackingClientKey, NULL);

void SetStackingClient(StackingClient* stacking_client) {
  Env::GetInstance()->set_stacking_client(stacking_client);
}

StackingClient* GetStackingClient() {
  return Env::GetInstance()->stacking_client();
}

void SetStackingClient(Window* window, StackingClient* stacking_client) {
  if (window) {
    RootWindow* root_window = window->GetRootWindow();
    DCHECK(root_window);
    root_window->SetProperty(kRootWindowStackingClientKey, stacking_client);
  } else {
    SetStackingClient(stacking_client);
  }
}

StackingClient* GetStackingClient(Window* window) {
  if (window) {
    RootWindow* root_window = window->GetRootWindow();
    DCHECK(root_window);
    StackingClient* root_window_stacking_client =
        root_window->GetProperty(kRootWindowStackingClientKey);
    if (root_window_stacking_client)
      return root_window_stacking_client;
  }
  return GetStackingClient();
}

}  // namespace client
}  // namespace aura
