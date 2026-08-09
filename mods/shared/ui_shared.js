// SPDX-License-Identifier: MIT
// Part of BeamNG FSR Bridge. See LICENSE at the repository root.
//
// Helpers shared by both product apps. Byte-identical in both mod ZIPs.
//
// The rule both UIs follow: never offer a control the hardware cannot honour,
// and never show a number that was not measured. Availability and the reason
// for unavailability both come from the native capability model, so the panel
// and the log can never disagree.
'use strict';

window.FsrShared = (function () {
  function features(status) {
    if (!status || !status.common || !status.common.environment) return [];
    return status.common.environment.features || [];
  }

  function findFeature(status, id) {
    var list = features(status);
    for (var i = 0; i < list.length; i++) {
      if (list[i].id === id) return list[i];
    }
    return null;
  }

  return {
    findFeature: findFeature,

    available: function (status, id) {
      var f = findFeature(status, id);
      return !!(f && f.available);
    },

    /// The sentence shown beside a disabled control. Written by the bridge to
    /// be read by a person, and displayed verbatim rather than summarised.
    reason: function (status, id) {
      var f = findFeature(status, id);
      return f ? f.reason : '';
    },

    ready: function (status) {
      return !!(status && status.common && status.common.bridgeLoaded &&
                status.common.initialised && status.moduleInstalled);
    },

    /// Renders a measured value, or an explicit "not measured" — never a zero
    /// that could be mistaken for a real reading.
    metric: function (value, unit, decimals) {
      if (value === undefined || value === null || value === 0) return 'not measured';
      return value.toFixed(decimals === undefined ? 1 : decimals) + (unit || '');
    },

    megabytes: function (value) {
      var n = parseInt(value, 10);
      if (!n) return 'not measured';
      return (n / (1024 * 1024)).toFixed(0) + ' MB';
    },

    notes: function (status, kind) {
      if (!status || !status.plan || !status.plan.notes) return [];
      return status.plan.notes[kind] || [];
    },

    /// Tells the user whether the *other* product is installed, without either
    /// product depending on it.
    companionInstalled: function (status, moduleId) {
      if (!status || !status.common || !status.common.installedModules) return false;
      var list = status.common.installedModules;
      for (var i = 0; i < list.length; i++) {
        if (list[i].id === moduleId) return !!list[i].installed;
      }
      return false;
    },

    versions: function (status) {
      return (status && status.versions) || [];
    },

    bindSetting: function (scope, luaModule, path) {
      return function (value) {
        bngApi.engineLua(luaModule + '.setSetting(' + bngApi.serializeToLua(path) + ', ' +
                         bngApi.serializeToLua(value) + ')');
      };
    }
  };
})();
