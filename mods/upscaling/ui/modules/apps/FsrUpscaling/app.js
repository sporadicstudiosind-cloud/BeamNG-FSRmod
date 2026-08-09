// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
//
// The AMD FSR Upscaling panel.
//
// Deliberately one screen with the four controls that matter at the top:
// on/off, implementation, quality and sharpness. Everything else is behind
// "Advanced", because the calibration knobs exist for the rare case where
// BeamNG's undocumented buffer encodings need correcting, not for everyday use.
'use strict';

angular.module('beamng.apps')
  .directive('fsrUpscaling', [function () {
    return {
      templateUrl: '/ui/modules/apps/FsrUpscaling/app.html',
      replace: true,
      restrict: 'EA',
      scope: true,
      link: function (scope) {
        // extensions.load('fsr_up_main') exposes extensions/fsr_up/main.lua
        // under this global.
        var MODULE = 'fsr_up_main';

        scope.settings = null;
        scope.status = null;
        scope.showAdvanced = false;
        scope.showDiagnostics = false;

        var S = window.FsrShared;

        scope.available = function (id) { return S.available(scope.status, id); };
        scope.reason = function (id) { return S.reason(scope.status, id); };
        scope.ready = function () { return S.ready(scope.status); };
        scope.installed = function () { return S.installed(scope.status); };
        scope.metric = S.metric;
        scope.megabytes = S.megabytes;
        scope.notes = function (kind) { return S.notes(scope.status, kind); };
        scope.versions = function () { return S.versions(scope.status); };

        scope.anyAvailable = function () {
          return scope.available('upscaling_analytical') || scope.available('upscaling_ml');
        };

        scope.frameGenInstalled = function () {
          return S.companionInstalled(scope.status, 'framegen');
        };

        scope.resolutionText = function () {
          if (!scope.status || !scope.status.plan) return 'unknown';
          var p = scope.status.plan;
          if (!p.renderWidth || !p.outputWidth) return 'unknown';
          return p.renderWidth + '×' + p.renderHeight + '  →  ' +
                 p.outputWidth + '×' + p.outputHeight;
        };

        /// BeamNG exposes no projection-jitter hook, so FSR runs without the
        /// sub-pixel jitter its temporal upscaler is designed around. The panel
        /// says so wherever upscaling is on rather than burying it in a README.
        scope.jitterAvailable = function () {
          return !!(scope.status && scope.status.jitterAvailable);
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
        scope.$on('FsrUpscalingStatus', function (event, status) {
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
