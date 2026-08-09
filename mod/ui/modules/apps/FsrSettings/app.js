// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
//
// The FSR settings and diagnostics app.
//
// Design rule enforced throughout: never offer a control the hardware cannot
// honour, and never show a number that was not measured. Options the GPU, API
// or OS rules out are disabled with the reason the native bridge gave — those
// reason strings are written to be read by a user, not parsed by a program, and
// they are displayed verbatim rather than summarised into something vaguer.
'use strict';

angular.module('beamng.apps')
  .directive('fsrSettings', ['$log', function ($log) {
    return {
      templateUrl: '/ui/modules/apps/FsrSettings/app.html',
      replace: true,
      restrict: 'EA',
      scope: true,
      link: function (scope) {
        scope.tab = 'fsr';
        scope.settings = null;
        scope.status = null;

        // Quality presets. Ratios are AMD's, from ffx_upscale.h. "Ultra
        // Quality" is deliberately absent: it is not a mode in the current FSR
        // API, and listing it would mean inventing a ratio AMD does not define.
        scope.qualityModes = [
          { id: 'nativeAA',         label: 'Native AA',        detail: '1.0x — no upscaling, temporal AA only' },
          { id: 'quality',          label: 'Quality',          detail: '1.5x per dimension' },
          { id: 'balanced',         label: 'Balanced',         detail: '1.7x per dimension' },
          { id: 'performance',      label: 'Performance',      detail: '2.0x per dimension' },
          { id: 'ultraPerformance', label: 'Ultra Performance', detail: '3.0x per dimension' },
          { id: 'custom',           label: 'Custom',           detail: 'Set the ratio manually' }
        ];

        scope.depthConventions = [
          { id: 'auto',      label: 'Auto (assume reversed-Z)' },
          { id: 'reversedZ', label: 'Reversed-Z' },
          { id: 'standardZ', label: 'Standard Z' }
        ];

        scope.logLevels = ['error', 'warn', 'info', 'debug', 'trace'];

        // --- capability helpers -------------------------------------------

        function findFeature(id) {
          if (!scope.status || !scope.status.environment ||
              !scope.status.environment.features) {
            return null;
          }
          var features = scope.status.environment.features;
          for (var i = 0; i < features.length; i++) {
            if (features[i].id === id) return features[i];
          }
          return null;
        }

        scope.featureAvailable = function (id) {
          var f = findFeature(id);
          return !!(f && f.available);
        };

        /// The explanation shown next to a disabled control. Comes from the
        /// native capability model so the UI and the log always agree.
        scope.featureReason = function (id) {
          var f = findFeature(id);
          return f ? f.reason : '';
        };

        scope.featureRequirement = function (id) {
          var f = findFeature(id);
          return f ? f.requirement : '';
        };

        scope.anyUpscalerAvailable = function () {
          return scope.featureAvailable('upscaling_analytical') ||
                 scope.featureAvailable('upscaling_ml');
        };

        scope.anyFrameGenAvailable = function () {
          return scope.featureAvailable('framegen_analytical') ||
                 scope.featureAvailable('framegen_ml');
        };

        /// Version lists come from AMD's own runtime query against this device.
        /// An empty list means AMD reports no provider — not that we forgot to
        /// populate it — so the UI says exactly that.
        scope.upscalerVersions = function () {
          return (scope.status && scope.status.upscalerVersions) || [];
        };
        scope.frameGenVersions = function () {
          return (scope.status && scope.status.frameGenVersions) || [];
        };

        // --- formatting ---------------------------------------------------

        /// Renders a measured value, or an explicit "not measured" rather than
        /// a zero that could be mistaken for a real reading.
        scope.metric = function (value, unit, decimals) {
          if (value === undefined || value === null || value === 0) return 'not measured';
          return value.toFixed(decimals === undefined ? 1 : decimals) + (unit || '');
        };

        scope.bytes = function (value) {
          var n = parseInt(value, 10);
          if (!n) return 'not measured';
          return (n / (1024 * 1024)).toFixed(0) + ' MB';
        };

        scope.resolutionText = function () {
          if (!scope.status || !scope.status.plan) return 'unknown';
          var p = scope.status.plan;
          if (!p.renderWidth || !p.outputWidth) return 'unknown';
          return p.renderWidth + '×' + p.renderHeight + '  →  ' +
                 p.outputWidth + '×' + p.outputHeight;
        };

        scope.planWarnings = function () {
          return (scope.status && scope.status.plan && scope.status.plan.warnings) || [];
        };
        scope.planDowngrades = function () {
          return (scope.status && scope.status.plan && scope.status.plan.downgrades) || [];
        };
        scope.planDecisions = function () {
          return (scope.status && scope.status.plan && scope.status.plan.decisions) || [];
        };

        scope.bridgeReady = function () {
          return !!(scope.status && scope.status.bridgeLoaded && scope.status.initialised);
        };

        // --- actions ------------------------------------------------------

        function pushSetting(key, value) {
          scope.settings[key] = value;
          bngApi.engineLua(
            'fsr_bridge.setSetting(' + bngApi.serializeToLua(key) + ', ' +
            bngApi.serializeToLua(value) + ')');
        }
        scope.pushSetting = pushSetting;

        scope.onToggle = function (key) {
          pushSetting(key, scope.settings[key]);
        };

        scope.onSelect = function (key) {
          pushSetting(key, scope.settings[key]);
        };

        scope.onSlider = function (key) {
          pushSetting(key, parseFloat(scope.settings[key]));
        };

        scope.resetSettings = function () {
          bngApi.engineLua('fsr_bridge.resetSettings()', function () {
            refreshSettings();
          });
        };

        scope.clearSafeMode = function () {
          bngApi.engineLua('fsr_bridge.clearSafeMode()', function () {
            refreshSettings();
          });
        };

        scope.exportDiagnostics = function () {
          bngApi.engineLua('fsr_bridge.exportDiagnostics()', function (path) {
            scope.exportedTo = path;
            scope.$evalAsync();
          });
        };

        // --- data flow ----------------------------------------------------

        function refreshSettings() {
          bngApi.engineLua('fsr_bridge.getSettings()', function (data) {
            if (data) {
              scope.settings = data;
              scope.$evalAsync();
            }
          });
        }

        function refreshStatus() {
          bngApi.engineLua('fsr_bridge.getStatus()', function (data) {
            if (data) {
              scope.status = data;
              scope.$evalAsync();
            }
          });
        }

        // The Lua half broadcasts whenever the bridge publishes a new status
        // document, so the panel updates without polling on its own.
        scope.$on('FsrStatusUpdated', function (event, status) {
          scope.status = status;
          scope.$evalAsync();
        });

        scope.$on('$destroy', function () {
          if (scope.pollHandle) { clearInterval(scope.pollHandle); }
        });

        refreshSettings();
        refreshStatus();
        // A slow fallback in case a status broadcast is missed; the broadcast
        // is the primary path.
        scope.pollHandle = setInterval(refreshStatus, 2000);

        $log.debug('[fsr] settings app initialised');
      }
    };
  }]);
