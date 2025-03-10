// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_DEVTOOLS_DEVTOOLS_WINDOW_H_
#define CHROME_BROWSER_DEVTOOLS_DEVTOOLS_WINDOW_H_

#include "chrome/browser/devtools/devtools_contents_resizing_strategy.h"
#include "chrome/browser/devtools/devtools_toggle_action.h"
#include "chrome/browser/devtools/devtools_ui_bindings.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/browser/web_contents_observer.h"

class Browser;
class BrowserWindow;
class DevToolsControllerTest;
class DevToolsEventForwarder;

namespace content {
class DevToolsAgentHost;
struct NativeWebKeyboardEvent;
class RenderViewHost;
}

namespace user_prefs {
class PrefRegistrySyncable;
}

class DevToolsWindow : public DevToolsUIBindings::Delegate,
                       public content::WebContentsDelegate {
 public:
  class ObserverWithAccessor : public content::WebContentsObserver {
   public:
    explicit ObserverWithAccessor(content::WebContents* web_contents);
    virtual ~ObserverWithAccessor();
    content::WebContents* GetWebContents();

   private:
    DISALLOW_COPY_AND_ASSIGN(ObserverWithAccessor);
  };

  static const char kDevToolsApp[];

  virtual ~DevToolsWindow();

  static void RegisterProfilePrefs(user_prefs::PrefRegistrySyncable* registry);

  // Return the DevToolsWindow for the given WebContents if one exists,
  // otherwise NULL.
  static DevToolsWindow* GetInstanceForInspectedWebContents(
      content::WebContents* inspected_web_contents);

  // Return the docked DevTools WebContents for the given inspected WebContents
  // if one exists and should be shown in browser window, otherwise NULL.
  // This method will return only fully initialized window ready to be
  // presented in UI.
  // If |out_strategy| is not NULL, it will contain resizing strategy.
  // For immediately-ready-to-use but maybe not yet fully initialized DevTools
  // use |GetInstanceForInspectedRenderViewHost| instead.
  static content::WebContents* GetInTabWebContents(
      content::WebContents* inspected_tab,
      DevToolsContentsResizingStrategy* out_strategy);

  static bool IsDevToolsWindow(content::WebContents* web_contents);

  // Open or reveal DevTools window, and perform the specified action.
  static DevToolsWindow* OpenDevToolsWindow(
      content::RenderViewHost* inspected_rvh,
      const DevToolsToggleAction& action);

  // Open or reveal DevTools window, with no special action.
  static DevToolsWindow* OpenDevToolsWindow(
      content::RenderViewHost* inspected_rvh);

  static DevToolsWindow* OpenDevToolsWindowForTest(
      content::RenderViewHost* inspected_rvh, bool is_docked);
  static DevToolsWindow* OpenDevToolsWindowForTest(
      Browser* browser, bool is_docked);

  // Perform specified action for current WebContents inside a |browser|.
  // This may close currently open DevTools window.
  static DevToolsWindow* ToggleDevToolsWindow(
      Browser* browser,
      const DevToolsToggleAction& action);

  // External frontend is always undocked.
  static void OpenExternalFrontend(
      Profile* profile,
      const std::string& frontend_uri,
      content::DevToolsAgentHost* agent_host);

  // Worker frontend is always undocked.
  static DevToolsWindow* OpenDevToolsWindowForWorker(
      Profile* profile,
      content::DevToolsAgentHost* worker_agent);

  static void InspectElement(
      content::RenderViewHost* inspected_rvh, int x, int y);

  Browser* browser_for_test() { return browser_; }
  content::WebContents* web_contents_for_test() { return main_web_contents_; }

  // Sets closure to be called after load is done. If already loaded, calls
  // closure immediately.
  void SetLoadCompletedCallback(const base::Closure& closure);

  // Forwards an unhandled keyboard event to the DevTools frontend.
  bool ForwardKeyboardEvent(const content::NativeWebKeyboardEvent& event);

  // BeforeUnload interception ////////////////////////////////////////////////

  // In order to preserve any edits the user may have made in devtools, the
  // beforeunload event of the inspected page is hooked - devtools gets the
  // first shot at handling beforeunload and presents a dialog to the user. If
  // the user accepts the dialog then the script is given a chance to handle
  // it. This way 2 dialogs may be displayed: one from the devtools asking the
  // user to confirm that they're ok with their devtools edits going away and
  // another from the webpage as the result of its beforeunload handler.
  // The following set of methods handle beforeunload event flow through
  // devtools window. When the |contents| with devtools opened on them are
  // getting closed, the following sequence of calls takes place:
  // 1. |DevToolsWindow::InterceptPageBeforeUnload| is called and indicates
  //    whether devtools intercept the beforeunload event.
  //    If InterceptPageBeforeUnload() returns true then the following steps
  //    will take place; otherwise only step 4 will be reached and none of the
  //    corresponding functions in steps 2 & 3 will get called.
  // 2. |DevToolsWindow::InterceptPageBeforeUnload| fires beforeunload event
  //    for devtools frontend, which will asynchronously call
  //    |WebContentsDelegate::BeforeUnloadFired| method.
  //    In case of docked devtools window, devtools are set as a delegate for
  //    its frontend, so method |DevToolsWindow::BeforeUnloadFired| will be
  //    called directly.
  //    If devtools window is undocked it's not set as the delegate so the call
  //    to BeforeUnloadFired is proxied through HandleBeforeUnload() rather
  //    than getting called directly.
  // 3a. If |DevToolsWindow::BeforeUnloadFired| is called with |proceed|=false
  //     it calls throught to the content's BeforeUnloadFired(), which from the
  //     WebContents perspective looks the same as the |content|'s own
  //     beforeunload dialog having had it's 'stay on this page' button clicked.
  // 3b. If |proceed| = true, then it fires beforeunload event on |contents|
  //     and everything proceeds as it normally would without the Devtools
  //     interception.
  // 4. If the user cancels the dialog put up by either the WebContents or
  //    devtools frontend, then |contents|'s |BeforeUnloadFired| callback is
  //    called with the proceed argument set to false, this causes
  //    |DevToolsWindow::OnPageCloseCancelled| to be called.

  // Devtools window in undocked state is not set as a delegate of
  // its frontend. Instead, an instance of browser is set as the delegate, and
  // thus beforeunload event callback from devtools frontend is not delivered
  // to the instance of devtools window, which is solely responsible for
  // managing custom beforeunload event flow.
  // This is a helper method to route callback from
  // |Browser::BeforeUnloadFired| back to |DevToolsWindow::BeforeUnloadFired|.
  // * |proceed| - true if the user clicked 'ok' in the beforeunload dialog,
  //   false otherwise.
  // * |proceed_to_fire_unload| - output parameter, whether we should continue
  //   to fire the unload event or stop things here.
  // Returns true if devtools window is in a state of intercepting beforeunload
  // event and if it will manage unload process on its own.
  static bool HandleBeforeUnload(content::WebContents* contents,
                                 bool proceed,
                                 bool* proceed_to_fire_unload);

  // Returns true if this contents beforeunload event was intercepted by
  // devtools and false otherwise. If the event was intercepted, caller should
  // not fire beforeunlaod event on |contents| itself as devtools window will
  // take care of it, otherwise caller should continue handling the event as
  // usual.
  static bool InterceptPageBeforeUnload(content::WebContents* contents);

  // Returns true if devtools browser has already fired its beforeunload event
  // as a result of beforeunload event interception.
  static bool HasFiredBeforeUnloadEventForDevToolsBrowser(Browser* browser);

  // Returns true if devtools window would like to hook beforeunload event
  // of this |contents|.
  static bool NeedsToInterceptBeforeUnload(content::WebContents* contents);

  // Notify devtools window that closing of |contents| was cancelled
  // by user.
  static void OnPageCloseCanceled(content::WebContents* contents);

 private:
  friend class DevToolsControllerTest;
  friend class DevToolsSanityTest;
  friend class BrowserWindowControllerTest;

  // DevTools lifecycle typically follows this way:
  // - Toggle/Open: client call;
  // - Create;
  // - ScheduleShow: setup window to be functional, but not yet show;
  // - DocumentOnLoadCompletedInMainFrame: frontend loaded;
  // - SetIsDocked: frontend decided on docking state;
  // - OnLoadCompleted: ready to present frontend;
  // - Show: actually placing frontend WebContents to a Browser or docked place;
  // - DoAction: perform action passed in Toggle/Open;
  // - ...;
  // - CloseWindow: initiates before unload handling;
  // - CloseContents: destroys frontend;
  // - DevToolsWindow is dead once it's main_web_contents dies.
  enum LifeStage {
    kNotLoaded,
    kOnLoadFired, // Implies SetIsDocked was not yet called.
    kIsDockedSet, // Implies DocumentOnLoadCompleted was not yet called.
    kLoadCompleted,
    kClosing
  };

  DevToolsWindow(Profile* profile,
                 const GURL& frontend_url,
                 content::RenderViewHost* inspected_rvh,
                 bool can_dock);

  static DevToolsWindow* Create(Profile* profile,
                                const GURL& frontend_url,
                                content::RenderViewHost* inspected_rvh,
                                bool shared_worker_frontend,
                                bool external_frontend,
                                bool can_dock);
  static GURL GetDevToolsURL(Profile* profile,
                             const GURL& base_url,
                             bool shared_worker_frontend,
                             bool external_frontend,
                             bool can_dock);
  static DevToolsWindow* FindDevToolsWindow(content::DevToolsAgentHost*);
  static DevToolsWindow* AsDevToolsWindow(content::WebContents*);
  static DevToolsWindow* CreateDevToolsWindowForWorker(Profile* profile);
  static DevToolsWindow* ToggleDevToolsWindow(
      content::RenderViewHost* inspected_rvh,
      bool force_open,
      const DevToolsToggleAction& action);

  static std::string GetDevToolsWindowPlacementPrefKey();

  // content::WebContentsDelegate:
  virtual content::WebContents* OpenURLFromTab(
      content::WebContents* source,
      const content::OpenURLParams& params) OVERRIDE;
  virtual void ActivateContents(content::WebContents* contents) OVERRIDE;
  virtual void AddNewContents(content::WebContents* source,
                              content::WebContents* new_contents,
                              WindowOpenDisposition disposition,
                              const gfx::Rect& initial_pos,
                              bool user_gesture,
                              bool* was_blocked) OVERRIDE;
  virtual void WebContentsCreated(content::WebContents* source_contents,
                                  int opener_render_frame_id,
                                  const base::string16& frame_name,
                                  const GURL& target_url,
                                  content::WebContents* new_contents) OVERRIDE;
  virtual void CloseContents(content::WebContents* source) OVERRIDE;
  virtual void ContentsZoomChange(bool zoom_in) OVERRIDE;
  virtual void BeforeUnloadFired(content::WebContents* tab,
                                 bool proceed,
                                 bool* proceed_to_fire_unload) OVERRIDE;
  virtual bool PreHandleKeyboardEvent(
      content::WebContents* source,
      const content::NativeWebKeyboardEvent& event,
      bool* is_keyboard_shortcut) OVERRIDE;
  virtual void HandleKeyboardEvent(
      content::WebContents* source,
      const content::NativeWebKeyboardEvent& event) OVERRIDE;
  virtual content::JavaScriptDialogManager*
      GetJavaScriptDialogManager() OVERRIDE;
  virtual content::ColorChooser* OpenColorChooser(
      content::WebContents* web_contents,
      SkColor color,
      const std::vector<content::ColorSuggestion>& suggestions) OVERRIDE;
  virtual void RunFileChooser(
      content::WebContents* web_contents,
      const content::FileChooserParams& params) OVERRIDE;
  virtual void WebContentsFocused(content::WebContents* contents) OVERRIDE;
  virtual bool PreHandleGestureEvent(
      content::WebContents* source,
      const blink::WebGestureEvent& event) OVERRIDE;

  // content::DevToolsUIBindings::Delegate overrides
  virtual void ActivateWindow() OVERRIDE;
  virtual void CloseWindow() OVERRIDE;
  virtual void SetInspectedPageBounds(const gfx::Rect& rect) OVERRIDE;
  virtual void SetContentsResizingStrategy(
      const gfx::Insets& insets, const gfx::Size& min_size) OVERRIDE;
  virtual void InspectElementCompleted() OVERRIDE;
  virtual void MoveWindow(int x, int y) OVERRIDE;
  virtual void SetIsDocked(bool is_docked) OVERRIDE;
  virtual void OpenInNewTab(const std::string& url) OVERRIDE;
  virtual void SetWhitelistedShortcuts(const std::string& message) OVERRIDE;
  virtual void InspectedContentsClosing() OVERRIDE;
  virtual void OnLoadCompleted() OVERRIDE;
  virtual InfoBarService* GetInfoBarService() OVERRIDE;
  virtual void RenderProcessGone() OVERRIDE;

  void CreateDevToolsBrowser();
  BrowserWindow* GetInspectedBrowserWindow();
  void ScheduleShow(const DevToolsToggleAction& action);
  void Show(const DevToolsToggleAction& action);
  void DoAction(const DevToolsToggleAction& action);
  void LoadCompleted();
  void SetIsDockedAndShowImmediatelyForTest(bool is_docked);
  void UpdateBrowserToolbar();
  void UpdateBrowserWindow();
  content::WebContents* GetInspectedWebContents();

  scoped_ptr<ObserverWithAccessor> inspected_contents_observer_;

  Profile* profile_;
  content::WebContents* main_web_contents_;
  content::WebContents* toolbox_web_contents_;
  DevToolsUIBindings* bindings_;
  Browser* browser_;
  bool is_docked_;
  const bool can_dock_;
  LifeStage life_stage_;
  DevToolsToggleAction action_on_load_;
  bool ignore_set_is_docked_;
  DevToolsContentsResizingStrategy contents_resizing_strategy_;
  // True if we're in the process of handling a beforeunload event originating
  // from the inspected webcontents, see InterceptPageBeforeUnload for details.
  bool intercepted_page_beforeunload_;
  base::Closure load_completed_callback_;

  base::TimeTicks inspect_element_start_time_;
  scoped_ptr<DevToolsEventForwarder> event_forwarder_;

  friend class DevToolsEventForwarder;
  DISALLOW_COPY_AND_ASSIGN(DevToolsWindow);
};

#endif  // CHROME_BROWSER_DEVTOOLS_DEVTOOLS_WINDOW_H_
