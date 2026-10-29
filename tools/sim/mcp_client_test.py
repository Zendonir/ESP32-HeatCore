#!/usr/bin/env python3
"""Interoperability test of the HeatCore MCP endpoint with the official MCP
Python SDK (pip install mcp) against the host simulator:

    ./build/host/heatcore_sim --demo --speed 600 --port 8099 &
    python3 tools/sim/mcp_client_test.py [--url http://127.0.0.1:8099]

Exits with status 1 if a check fails. Works the same against a real
controller (--url http://heatcore.local --token <token> --no-setup).
"""
import argparse
import asyncio
import json
import sys
import urllib.error
import urllib.request

from mcp import Client
from mcp.client.streamable_http import streamable_http_client
from mcp.shared._httpx_utils import create_mcp_http_client

failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def http(url, method="GET", body=None, headers=None):
    req = urllib.request.Request(url, method=method, data=None if body is None else json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json", **(headers or {})})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


def text_json(result):
    return json.loads(result.content[0].text)


async def run(base, token):
    transport = streamable_http_client(base + "/mcp", http_client=create_mcp_http_client(
        headers={"Authorization": "Bearer " + token}))
    async with Client(transport, mode="auto") as c:
        print("negotiated protocol:", c.protocol_version, "server:", c.server_info)
        check(c.protocol_version.startswith("2025") or c.protocol_version.startswith("2024"),
              "fallback from server/discover to the initialize handshake")
        tools = await c.list_tools()
        names = [t.name for t in tools.tools]
        print("tools:", ", ".join(names))
        check("get_statistics" in names and "change_settings" in names, "tool list (approve access)")
        ov = text_json(await c.call_tool("get_overview", {}))
        check(len(ov["rooms"]) >= 1, "get_overview returns rooms")
        room = ov["rooms"][0]["id"]
        st = text_json(await c.call_tool("get_statistics", {"days": 1}))
        check("total" in st["rooms"][0], "get_statistics returns KPIs")
        hist = text_json(await c.call_tool("get_room_history", {"room": room, "hours": 2}))
        check(len(hist["data"]["t"]) >= 1, "get_room_history returns points (%d)" % len(hist["data"]["t"]))
        settings = text_json(await c.call_tool("get_settings", {}))
        comfort = settings["rooms"][0]["comfort"]
        dry = await c.call_tool("change_settings", {
            "changes": [{"type": "room", "room": room, "comfort": round(comfort + 0.5, 1)}],
            "reason": "Interop-Test", "dry_run": True})
        check(not dry.is_error and text_json(dry)["status"] == "dry_run", "change_settings dry_run")
        bad = await c.call_tool("change_settings", {
            "changes": [{"type": "room", "room": room, "comfort": comfort + 5}], "reason": "zu gross"})
        check(bad.is_error, "out-of-range change is rejected: " + bad.content[0].text[:80])
        prop = await c.call_tool("change_settings", {
            "changes": [{"type": "room", "room": room, "comfort": round(comfort + 0.5, 1)}],
            "reason": "Interop-Test"})
        res = text_json(prop)
        check(res["status"] == "pending", "proposal waits for approval (id %s)" % res.get("id"))
        prompts = await c.list_prompts()
        check(any(p.name == "optimize_heating" for p in prompts.prompts), "prompt optimize_heating listed")
        pr = await c.get_prompt("optimize_heating", {"room": room})
        check("get_statistics" in pr.messages[0].content.text, "prompt text")
        return res.get("id")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://127.0.0.1:8099")
    ap.add_argument("--token", default="interop-test-token-123456")
    ap.add_argument("--no-setup", action="store_true", help="do not change the AI access settings via REST")
    a = ap.parse_args()
    base = a.url.rstrip("/")
    if not a.no_setup:
        st, body = http(base + "/api/ai", "PUT", {"access": "approve", "token": a.token})
        check(st == 200, "enable MCP (approve) via REST: %s" % body)
    st, _ = http(base + "/mcp", "POST", {"jsonrpc": "2.0", "id": 1, "method": "ping"},
                 {"Authorization": "Bearer falsch"})
    check(st == 401, "wrong token -> 401")
    st, _ = http(base + "/mcp", "POST", {"jsonrpc": "2.0", "id": 1, "method": "ping"},
                 {"Authorization": "Bearer " + a.token, "Origin": "http://evil.example"})
    check(st == 403, "foreign Origin -> 403")
    pid = asyncio.run(run(base, a.token))
    if not a.no_setup and pid:
        st, body = http(base + "/api/ai/changes/%s/accept" % pid, "POST", {})
        check(st == 200 and '"applied"' in body, "proposal accepted in the web API")
    print("\n%d failure(s)" % len(failures))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
