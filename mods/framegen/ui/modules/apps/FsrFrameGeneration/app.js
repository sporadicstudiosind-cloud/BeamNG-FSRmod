// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
//
// The AMD FSR Frame Generation panel.
//
// Deliberately one screen with the three controls that matter at the top:
// on/off, implementation, Anti-Lag. Everything else is behind "Advanced",
// because the calibration knobs exist for the rare case where BeamNG's
// undocumented buffer encodings need correcting, not for everyday use.
'use strict';

angular.module('beamng.apps')
  .directive('fsrFrameGeneration', [function () {
    return {
      templateUrl: '/ui/modules/apps/FsrFrameGeneration/app.html',
      replace: true,
      restrict: 'EA',
      scope: true,
      link: function (scope) {
        // extensions.load('fsr_fg_main') exposes extensions/fsr_fg/main.lua
        // under this global.
        var MODULE = 'fsr_fg_main';

        scope.settings = null;
        scope.status = null;
        scope.showAdvanced = false;
        scope.showDiagnostics = false;

        var S = window.FsrShared;

        scope.available = function (id) { return S.available(scope.status, id); };
        scope.reason = function (id) { return S.reason(scope.status, id); };
        scope.ready = function () { return S.ready(scope.status); };
        scope.metric = S.metric;
        scope.megabytes = S.megabytes;
        scope.notes = function (kind) { return S.notes(scope.status, kind); };
        scope.versions = function () { return S.versions(scope.status); };

        scope.anyAvailable = function () {
          return scope.available('framegen_analytical') || scope.available('framegen_ml');
        };

        scope.upscalingInstalled = function () {
          return S.companionInstalled(scope.status, 'upscaling');
        };

        /// AMD's own documented minimum base frame rate for whichever
        /// implementation is running: 60 fps for 3.1.6, 30 fps for 4.0.1.
        scope.amdMinimumFps = function () {
          if (!scope.status || !scope.status.plan) return 0;
          return scope.status.plan.amdMinimumBaseFps || 0;
        };

        scope.belowAmdMinimum = function () {
          var minimum = scope.amdMinimumFps();
          if (!minimum || !scope.status || !scope.status.common) return false;
          var base = scope.status.common.renderedFps;
          return base > 0 && base < minimum;
        };

        /// AMD's SDK 2.3.0 gives an application no way to request more than one
        /// generated frame — `numGeneratedFrames` arrives already decided on the
        /// dispatch callback. So this reports what the runtime actually did
        /// rather than offering a multiplier that would not be honoured.
        scope.multiplierText = function () {
          if (!scope.status) return 'not measured';
          var generated = scope.status.observedGeneratedPerFrame;
          if (!generated) return 'not measured';
          return (generated + 1) + '×  (' + generated +
                 ' generated per rendered frame)';
        };

        scope.set = function (path, value) {
          bngApi.engineLua(MODULE + '.setSetting(' + bngApi.serializeToLua(path) + ', ' +
                           bngApi.serializeToLua(value) + ')');
        };

        scope.onChange = function (path) {
          // Angular has already written the new value into scope.settings.
          var target = scope.settings;
          var keys = path.split('.');
          for (var i = 0; i < keys.length - 1; i++) target = target[keys[i]];
          scope.set(path, target[keys[keys.length - 1]]);
        };

        scope.onSlider = function (path) {
          var target = scope.settings;
          var keys = path.split('.');
          for (var i = 0; i < keys.length - 1; i++) target = target[keys[i]];
          scope.set(path, parseFloat(target[keys[keys.length - 1]]));
        };

        scope.reset = function () {
          bngApi.engineLua(MODULE + '.resetSettings()', refreshSettings);
        };
        scope.clearSafeMode = function () {
          bngApi.engineLua(MODULE + '.clearSafeMode()', refreshSettings);
        };
        scope.exportDiagnostics = function () {
          bngApi.engineLua(MODULE + '.exportDiagnostics()', function (path) {
            scope.exportedTo = path;
            scope.$evalAsync();
          });
        };

        function refreshSettings() {
          bngApi.engineLua(MODULE + '.getSettings()', function (data) {
            if (data) { scope.settings = data; scope.$evalAsync(); }
          });
        }

        function refreshStatus() {
          bngApi.engineLua(MODULE + '.getStatus()', function (data) {
            if (data) { scope.status = data; scope.$evalAsync(); }
          });
        }

        // The Lua half broadcasts on every new status document, so the panel
        // updates without polling. The interval below is only a fallback.
        scope.$on('FsrFrameGenStatus', function (event, status) {
          scope.status = status;
          scope.$evalAsync();
        });

        scope.$on('$destroy', function () {
          if (scope.pollHandle) clearInterval(scope.pollHandle);
        });

        refreshSettings();
        refreshStatus();
        scope.pollHandle = setInterval(refreshStatus, 2000);
      }
    };
  }]);
