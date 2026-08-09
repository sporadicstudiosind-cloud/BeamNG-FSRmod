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

    /// True only when this product is *running*, not merely installed.
    ///
    /// The distinction matters: the bridge can be loaded and the product
    /// installed while the resolved plan is inactive, in which case nothing is
    /// executing. Reporting "active" then would be the panel claiming a result
    /// it does not have. `installed()` covers the weaker question.
    ready: function (status) {
      return !!(status && status.common && status.common.bridgeLoaded &&
                status.common.initialised && status.moduleInstalled &&
                status.plan && status.plan.active);
    },

    /// The bridge is up and this product's half of it started. Used to decide
    /// whether the diagnostics are worth showing, not whether FSR is running.
    installed: function (status) {
      return !!(status && status.common && status.common.bridgeLoaded &&
                status.common.initialised && status.moduleInstalled);
    },

    /// Metrics are nested under common.metrics in the status document. Reading
    /// them through here rather than inline stops a status that predates a
    /// field from throwing on a missing intermediate object.
    metrics: function (status) {
      return (status && status.common && status.common.metrics) || {};
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
