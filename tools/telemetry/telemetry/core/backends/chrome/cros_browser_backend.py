# Copyright 2013 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import logging
import os

from telemetry import decorators

from telemetry.core import exceptions
from telemetry.core import forwarders
from telemetry.core import util
from telemetry.core.backends.chrome import chrome_browser_backend
from telemetry.core.backends.chrome import misc_web_contents_backend
from telemetry.core.forwarders import cros_forwarder


class CrOSBrowserBackend(chrome_browser_backend.ChromeBrowserBackend):
  def __init__(self, browser_options, cri, is_guest, extensions_to_load):
    super(CrOSBrowserBackend, self).__init__(
        supports_tab_control=True, supports_extensions=not is_guest,
        browser_options=browser_options,
        output_profile_path=None, extensions_to_load=extensions_to_load)

    # Initialize fields so that an explosion during init doesn't break in Close.
    self._cri = cri
    self._is_guest = is_guest
    self._forwarder = None

    from telemetry.core.backends.chrome import chrome_browser_options
    assert isinstance(browser_options,
                      chrome_browser_options.CrosBrowserOptions)

    self.wpr_port_pairs = forwarders.PortPairs(
        http=forwarders.PortPair(self.wpr_port_pairs.http.local_port,
                                 self.GetRemotePort(
                                     self.wpr_port_pairs.http.local_port)),
        https=forwarders.PortPair(self.wpr_port_pairs.https.local_port,
                                  self.GetRemotePort(
                                      self.wpr_port_pairs.http.local_port)),
        dns=None)
    self._remote_debugging_port = self._cri.GetRemotePort()
    self._port = self._remote_debugging_port

    # Copy extensions to temp directories on the device.
    # Note that we also perform this copy locally to ensure that
    # the owner of the extensions is set to chronos.
    for e in extensions_to_load:
      extension_dir = cri.RunCmdOnDevice(
          ['mktemp', '-d', '/tmp/extension_XXXXX'])[0].rstrip()
      cri.PushFile(e.path, extension_dir)
      cri.Chown(extension_dir)
      e.local_path = os.path.join(extension_dir, os.path.basename(e.path))

    self._cri.RestartUI(self.browser_options.clear_enterprise_policy)
    util.WaitFor(self.IsBrowserRunning, 20)

    # Delete test user's cryptohome vault (user data directory).
    if not self.browser_options.dont_override_profile:
      self._cri.RunCmdOnDevice(['cryptohome', '--action=remove', '--force',
                                '--user=%s' % self._username])
    if self.browser_options.profile_dir:
      cri.RmRF(self.profile_directory)
      cri.PushFile(self.browser_options.profile_dir + '/Default',
                   self.profile_directory)
      cri.Chown(self.profile_directory)

  def GetBrowserStartupArgs(self):
    args = super(CrOSBrowserBackend, self).GetBrowserStartupArgs()
    args.extend([
            '--enable-smooth-scrolling',
            '--enable-threaded-compositing',
            '--enable-per-tile-painting',
            # Disables the start page, as well as other external apps that can
            # steal focus or make measurements inconsistent.
            '--disable-default-apps',
            # Allow devtools to connect to chrome.
            '--remote-debugging-port=%i' % self._remote_debugging_port,
            # Open a maximized window.
            '--start-maximized',
            # Debug logging.
            '--vmodule=*/chromeos/net/*=2,*/chromeos/login/*=2'])

    if not self.browser_options.gaia_login:
      # Skip user image selection screen, and post login screens.
      args.append('--oobe-skip-postlogin')

    return args

  @property
  def pid(self):
    return self._cri.GetChromePid()

  @property
  def browser_directory(self):
    result = self._cri.GetChromeProcess()
    if result and 'path' in result:
      return os.path.dirname(result['path'])
    return None

  @property
  def profile_directory(self):
    return '/home/chronos/Default'

  def GetRemotePort(self, port):
    if self._cri.local:
      return port
    return self._cri.GetRemotePort()

  def __del__(self):
    self.Close()

  def Start(self):
    # Escape all commas in the startup arguments we pass to Chrome
    # because dbus-send delimits array elements by commas
    startup_args = [a.replace(',', '\\,') for a in self.GetBrowserStartupArgs()]

    # Restart Chrome with the login extension and remote debugging.
    logging.info('Restarting Chrome with flags and login')
    args = ['dbus-send', '--system', '--type=method_call',
            '--dest=org.chromium.SessionManager',
            '/org/chromium/SessionManager',
            'org.chromium.SessionManagerInterface.EnableChromeTesting',
            'boolean:true',
            'array:string:"%s"' % ','.join(startup_args)]
    self._cri.RunCmdOnDevice(args)

    if not self._cri.local:
      self._port = util.GetUnreservedAvailableLocalPort()
      self._forwarder = self.forwarder_factory.Create(
          forwarders.PortPairs(
              http=forwarders.PortPair(self._port, self._remote_debugging_port),
              https=None,
              dns=None), forwarding_flag='L')

    # Wait for oobe.
    self._WaitForBrowserToComeUp(wait_for_extensions=False)
    util.WaitFor(lambda: self.oobe_exists, 10)

    if self.browser_options.auto_login:
      try:
        if self._is_guest:
          pid = self.pid
          self.oobe.NavigateGuestLogin()
          # Guest browsing shuts down the current browser and launches an
          # incognito browser in a separate process, which we need to wait for.
          util.WaitFor(lambda: pid != self.pid, 10)
        elif self.browser_options.gaia_login:
          self.oobe.NavigateGaiaLogin(self._username, self._password)
        else:
          self.oobe.NavigateFakeLogin(self._username, self._password)
        self._WaitForLogin()
      except util.TimeoutException:
        self._cri.TakeScreenShot('login-screen')
        raise exceptions.LoginException('Timed out going through login screen')

    logging.info('Browser is up!')

  def Close(self):
    super(CrOSBrowserBackend, self).Close()

    if self._cri:
      self._cri.RestartUI(False) # Logs out.

    util.WaitFor(lambda: not self._IsCryptohomeMounted(), 30)

    if self._forwarder:
      self._forwarder.Close()
      self._forwarder = None

    if self._cri:
      for e in self._extensions_to_load:
        self._cri.RmRF(os.path.dirname(e.local_path))

    self._cri = None

  @property
  @decorators.Cache
  def forwarder_factory(self):
    return cros_forwarder.CrOsForwarderFactory(self._cri)

  def IsBrowserRunning(self):
    return bool(self.pid)

  def GetStandardOutput(self):
    return 'Cannot get standard output on CrOS'

  def GetStackTrace(self):
    return 'Cannot get stack trace on CrOS'

  @property
  @decorators.Cache
  def misc_web_contents_backend(self):
    """Access to chrome://oobe/login page."""
    return misc_web_contents_backend.MiscWebContentsBackend(self)

  @property
  def oobe(self):
    return self.misc_web_contents_backend.GetOobe()

  @property
  def oobe_exists(self):
    return self.misc_web_contents_backend.oobe_exists

  @property
  def _username(self):
    return self.browser_options.username

  @property
  def _password(self):
    return self.browser_options.password

  def _IsCryptohomeMounted(self):
    username = '$guest' if self._is_guest else self._username
    return self._cri.IsCryptohomeMounted(username, self._is_guest)

  def _IsLoggedIn(self):
    """Returns True if cryptohome has mounted, the browser is
    responsive to devtools requests, and the oobe has been dismissed."""
    return (self._IsCryptohomeMounted() and
            self.HasBrowserFinishedLaunching() and
            not self.oobe_exists)

  def _WaitForLogin(self):
    # Wait for cryptohome to mount.
    util.WaitFor(self._IsLoggedIn, 60)

    # Wait for extensions to load.
    self._WaitForBrowserToComeUp()

    # Workaround for crbug.com/374462 - the bug doesn't manifest in the guest
    # session, which also starts with an open browser tab.
    retries = 3
    while not self._is_guest:
      try:
        # Open a new window/tab.
        tab = self.tab_list_backend.New(timeout=30)
        tab.Navigate('about:blank', timeout=10)
        break
      except (exceptions.TabCrashException, util.TimeoutException,
              IndexError):
        retries -= 1
        logging.warning('TabCrashException/TimeoutException in '
                        'new tab creation/navigation, '
                        'remaining retries %d', retries)
        if not retries:
          raise
