"use strict";
const {readFileSync} = require("node:fs");
const {runInNewContext} = require("node:vm");
const {test} = require("node:test");
const assert = require("node:assert/strict");
const {join} = require("node:path");

// Load the pure history class without starting the DOM-bound dashboard.
const source = readFileSync(join(__dirname, "../static/app.js"), "utf8");
const WarningHistory = runInNewContext(source.slice(source.indexOf("class WarningHistory"),
  source.indexOf("const $ =")) + "\nWarningHistory");
const sample = (sampleId, endpointId = "process-a", warning = true) => ({
  endpointId, sampleId, name:endpointId, pid:123,
  checks:warning ? [{key:"errors", state:"warn", counters:{queue_full_drops:"35"}}] : [],
  changes:{interval_seconds:5}
});

test("same sample does not duplicate, reset age or resurrect a dismissed warning", () => {
  let now = 0;
  const history = new WarningHistory(() => now);
  history.observe([sample("one")]);
  const first = history.list()[0];
  now = 120000;
  history.observe([sample("one")]);
  assert.equal(history.list().length, 1);
  assert.equal(history.age(first), 120);
  history.dismiss(first.id);
  history.observe([sample("one")]);
  assert.equal(history.list().length, 0);
  history.observe([sample("two")]);
  assert.equal(history.list().length, 1);
});

test("keeps at most five warnings globally, newest first", () => {
  let now = 0;
  const history = new WarningHistory(() => now);
  for (let i = 0; i < 8; i++) {
    now += 5000;
    history.observe([sample(String(i), "process-a"), sample(String(i), "process-b")]);
    assert.ok(history.list().length <= 5);
  }
  assert.equal(history.list().length, 5);
  assert.equal(history.list()[0].sampleId, "7");
  assert.equal(history.list()[4].sampleId, "5");
});

test("recovery and process disappearance retain evidence until the five-minute boundary", () => {
  let now = 0;
  const history = new WarningHistory(() => now);
  history.observe([sample("warning")]);
  now = 5000;
  history.observe([sample("healthy", "process-a", false)]);
  assert.equal(history.list().length, 1);
  assert.equal(history.list()[0].checks[0].counters.queue_full_drops, "35");
  history.observe([]);
  assert.equal(history.list().length, 1);
  assert.equal(history.seen.size, 0);
  now = 299999;
  assert.equal(history.list().length, 1);
  now = 300000;
  assert.equal(history.list().length, 0);
});

test("expired warning does not return when the same snapshot is fetched again", () => {
  let now = 0;
  const history = new WarningHistory(() => now);
  history.observe([sample("one")]);
  now = 301000;
  history.observe([sample("one")]);
  assert.equal(history.list().length, 0);
  history.observe([sample("two", "process-a", false)]);
  history.observe([sample("three")]);
  assert.equal(history.list().length, 1);
  assert.equal(history.list()[0].sampleId, "three");
});
