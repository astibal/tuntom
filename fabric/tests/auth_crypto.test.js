"use strict";
const {readFileSync}=require("node:fs");
const {join}=require("node:path");
const {runInNewContext}=require("node:vm");
const {pbkdf2Sync,createHash,createHmac}=require("node:crypto");
const {test}=require("node:test");
const assert=require("node:assert/strict");

const source=readFileSync(join(__dirname,"../static/vendor/noble-auth.js"),"utf8");
const noble=runInNewContext(source+";TuntomCrypto",{setTimeout});
const bytes=value=>new TextEncoder().encode(value);
const hex=value=>Buffer.from(value).toString("hex");

test("vendored HTTP crypto matches platform SHA-256, HMAC and PBKDF2",async()=>{
  assert.equal(hex(noble.digest(bytes("fabric"))),createHash("sha256").update("fabric").digest("hex"));
  assert.equal(hex(noble.mac(bytes("key"),bytes("fabric"))),createHmac("sha256","key").update("fabric").digest("hex"));
  assert.equal(hex(await noble.pbkdf2(bytes("password"),bytes("0123456789abcdef"),1000)),
    pbkdf2Sync("password","0123456789abcdef",1000,32,"sha256").toString("hex"));
});
