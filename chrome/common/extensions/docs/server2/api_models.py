# Copyright 2013 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import posixpath

from compiled_file_system import SingleFile, Unicode
from docs_server_utils import StringIdentity
from extensions_paths import API_PATHS
from features_bundle import HasParentFeature
from file_system import FileNotFoundError
from future import Collect, Future
from platform_util import PlatformToExtensionType
from schema_util import ProcessSchema
from third_party.json_schema_compiler.json_schema import DeleteNodes
from third_party.json_schema_compiler.model import Namespace, UnixName


class APIModels(object):
  '''Tracks APIs and their Models.
  '''

  def __init__(self,
               features_bundle,
               compiled_fs_factory,
               file_system,
               platform):
    self._features_bundle = features_bundle
    self._platform = PlatformToExtensionType(platform)
    self._model_cache = compiled_fs_factory.Create(
        file_system, self._CreateAPIModel, APIModels, category=self._platform)

  @SingleFile
  @Unicode
  def _CreateAPIModel(self, path, data):
    def does_not_include_platform(node):
      return ('extension_types' in node and
              node['extension_types'] != 'all' and
              self._platform not in node['extension_types'])

    schema = ProcessSchema(path, data, inline=True)[0]
    if not schema:
      raise ValueError('No schema for %s' % path)
    return Namespace(DeleteNodes(
        schema, matcher=does_not_include_platform), schema['namespace'])

  def GetNames(self):
    # API names appear alongside some of their methods/events/etc in the
    # features file. APIs are those which either implicitly or explicitly have
    # no parent feature (e.g. app, app.window, and devtools.inspectedWindow are
    # APIs; runtime.onConnectNative is not).
    api_features = self._features_bundle.GetAPIFeatures().Get()
    return [name for name, feature in api_features.iteritems()
            if not HasParentFeature(name, feature, api_features)]

  def GetModel(self, api_name):
    # By default |api_name| is assumed to be given without a path or extension,
    # so combinations of known paths and extension types will be searched.
    api_extensions = ('.json', '.idl')
    api_paths = API_PATHS

    # Callers sometimes include a file extension and/or prefix path with the
    # |api_name| argument. We believe them and narrow the search space
    # accordingly.
    name, ext = posixpath.splitext(api_name)
    if ext in api_extensions:
      api_extensions = (ext,)
      api_name = name
    for api_path in api_paths:
      if api_name.startswith(api_path):
        api_name = api_name[len(api_path):]
        api_paths = (api_path,)
        break

    # API names are given as declarativeContent and app.window but file names
    # will be declarative_content and app_window.
    file_name = UnixName(api_name).replace('.', '_')
    # Devtools APIs are in API/devtools/ not API/, and have their
    # "devtools" names removed from the file names.
    basename = posixpath.basename(file_name)
    if 'devtools_' in basename:
      file_name = posixpath.join(
          'devtools', file_name.replace(basename,
                                        basename.replace('devtools_' , '')))

    futures = [self._model_cache.GetFromFile(
                   posixpath.join(path, '%s%s' % (file_name, ext)))
               for ext in api_extensions
               for path in api_paths]
    def resolve():
      for future in futures:
        try:
          return future.Get()
        # Either the file wasn't found or there was no schema for the file
        except (FileNotFoundError, ValueError): pass
      # Propagate the first error if neither were found.
      futures[0].Get()
    return Future(callback=resolve)

  def Cron(self):
    futures = [self.GetModel(name) for name in self.GetNames()]
    return Collect(futures, except_pass=(FileNotFoundError, ValueError))

  def IterModels(self):
    future_models = [(name, self.GetModel(name)) for name in self.GetNames()]
    for name, future_model in future_models:
      try:
        model = future_model.Get()
      except FileNotFoundError:
        continue
      if model:
        yield name, model
