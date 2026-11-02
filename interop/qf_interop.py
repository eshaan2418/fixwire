"""Interoperability tests against QuickFIX (the reference open-source FIX engine, Python bindings).

QuickFIX runs with its FIX 4.4 data dictionary validation ON, so every message fixwire sends must
pass QuickFIX's required-field, type and format checks.

  A. fixwire-client (initiator)  -> QuickFIX acceptor, which fills every order
  B. QuickFIX initiator          -> fixwire-exchange (acceptor)
  C. fixwire-client with a file store crashes mid-stream, restarts, and recovers the fills it
     missed through ResendRequest/PossDup against QuickFIX's own store

usage: python interop/qf_interop.py BUILD_DIR OUT_JSON
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

import quickfix as fix

DICT = Path(fix.__file__).parent.parent.parent.parent / "share" / "quickfix" / "FIX44.xml"


def settings(kind: str, port: int, sender: str, target: str, store: str, log: str) -> fix.SessionSettings:
    cfg = f"""[DEFAULT]
ConnectionType={kind}
StartTime=00:00:00
EndTime=23:59:59
HeartBtInt=30
ReconnectInterval=1
FileStorePath={store}
FileLogPath={log}
UseDataDictionary=Y
DataDictionary={DICT}
ValidateUserDefinedFields=N
[SESSION]
BeginString=FIX.4.4
SenderCompID={sender}
TargetCompID={target}
{"SocketAcceptPort" if kind == "acceptor" else "SocketConnectPort"}={port}
{"" if kind == "acceptor" else "SocketConnectHost=127.0.0.1"}
"""
    return fix.SessionSettings(_tmpfile(cfg))


def _tmpfile(text: str) -> str:
    f = tempfile.NamedTemporaryFile("w", suffix=".cfg", delete=False)
    f.write(text)
    f.close()
    return f.name


class Filler(fix.Application):
    """QuickFIX acceptor application: New + Fill execution reports for each NewOrderSingle."""

    def __init__(self):
        super().__init__()
        self.orders = 0
        self.ids = set()
        self.rejects = 0
        self.exec_id = 0
        self.lock = threading.Lock()

    def onCreate(self, sessionID): pass  # noqa: E704,N802
    def onLogon(self, sessionID): pass  # noqa: E704,N802
    def onLogout(self, sessionID): pass  # noqa: E704,N802
    def toAdmin(self, message, sessionID): pass  # noqa: E704,N802

    def fromAdmin(self, message, sessionID):  # noqa: N802
        t = fix.MsgType()
        message.getHeader().getField(t)
        if t.getValue() == "3":
            self.rejects += 1

    def toApp(self, message, sessionID): pass  # noqa: E704,N802

    def fromApp(self, message, sessionID):  # noqa: N802
        t = fix.MsgType()
        message.getHeader().getField(t)
        if t.getValue() != "D":
            return
        cl = fix.ClOrdID()
        message.getField(cl)
        sym, side, qty, px = fix.Symbol(), fix.Side(), fix.OrderQty(), fix.Price()
        message.getField(sym)
        message.getField(side)
        message.getField(qty)
        message.getField(px)
        with self.lock:
            self.orders += 1
            self.ids.add(cl.getValue())
        for k in (0, 1):
            with self.lock:
                self.exec_id += 1
                eid = self.exec_id
            er = fix.Message()
            er.getHeader().setField(fix.MsgType("8"))
            er.setField(fix.OrderID("o" + cl.getValue()))
            er.setField(fix.ClOrdID(cl.getValue()))
            er.setField(fix.ExecID(str(eid)))
            er.setField(fix.ExecType("0" if k == 0 else "F"))
            er.setField(fix.OrdStatus("0" if k == 0 else "2"))
            er.setField(sym)
            er.setField(side)
            er.setField(qty)
            er.setField(fix.LeavesQty(qty.getValue() if k == 0 else 0))
            er.setField(fix.CumQty(0 if k == 0 else qty.getValue()))
            er.setField(fix.AvgPx(0 if k == 0 else px.getValue()))
            if k == 1:
                er.setField(fix.LastPx(px.getValue()))
                er.setField(fix.LastQty(qty.getValue()))
            fix.Session.sendToTarget(er, sessionID)


class Sender(fix.Application):
    """QuickFIX initiator application: sends N orders after logon and counts fills."""

    def __init__(self, n: int):
        super().__init__()
        self.n, self.fills, self.rejects, self.logged_on = n, set(), 0, threading.Event()
        self.lock = threading.Lock()

    def onCreate(self, sessionID): pass  # noqa: E704,N802
    def onLogout(self, sessionID): pass  # noqa: E704,N802
    def toAdmin(self, message, sessionID): pass  # noqa: E704,N802
    def toApp(self, message, sessionID): pass  # noqa: E704,N802

    def onLogon(self, sessionID):  # noqa: N802
        self.sid = sessionID
        self.logged_on.set()

    def fromAdmin(self, message, sessionID):  # noqa: N802
        t = fix.MsgType()
        message.getHeader().getField(t)
        if t.getValue() == "3":
            self.rejects += 1

    def fromApp(self, message, sessionID):  # noqa: N802
        et = fix.ExecType()
        message.getField(et)
        if et.getValue() == "F":
            cl = fix.ClOrdID()
            message.getField(cl)
            with self.lock:
                self.fills.add(cl.getValue())


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=300, **kw)
    if r.returncode != 0:
        raise RuntimeError(f"{cmd}: {r.stderr}")
    return json.loads(r.stdout.strip().splitlines()[-1])


def scenario_a(build: Path, work: Path, port: int, n: int):
    app = Filler()
    s = settings("acceptor", port, "EXCH", "CLIENT", str(work / "qfa_store"), str(work / "qfa_log"))
    acc = fix.SocketAcceptor(app, fix.FileStoreFactory(s), s, fix.FileLogFactory(s))
    acc.start()
    time.sleep(0.5)
    t0 = time.time()
    res = run([str(build / "fixwire-client"), "--port", str(port), "--orders", str(n), "--window", "32", "--prefix", "A"])
    secs = time.time() - t0
    time.sleep(0.3)
    acc.stop()
    return {"orders_sent": res["orders_sent"], "quickfix_orders_accepted": app.orders, "unique_ids": len(app.ids),
            "fills_received_by_fixwire": res["fills_received"], "quickfix_session_rejects_received": app.rejects,
            "duplicate_fill_reports": res["duplicate_fill_reports"], "seconds": secs, "client_rtt_us": res["rtt_us"]}


def scenario_b(build: Path, work: Path, port: int, n: int):
    ex = subprocess.Popen([str(build / "fixwire-exchange"), "--port", str(port), "--once"], stderr=subprocess.PIPE, text=True)
    time.sleep(0.5)
    app = Sender(n)
    s = settings("initiator", port, "CLIENT", "EXCH", str(work / "qfi_store"), str(work / "qfi_log"))
    ini = fix.SocketInitiator(app, fix.FileStoreFactory(s), s, fix.FileLogFactory(s))
    ini.start()
    if not app.logged_on.wait(10):
        raise RuntimeError("QuickFIX initiator did not log on to fixwire-exchange")
    t0 = time.time()
    for i in range(n):
        o = fix.Message()
        o.getHeader().setField(fix.MsgType("D"))
        o.setField(fix.ClOrdID(f"B{i}"))
        o.setField(fix.Symbol("MSFT"))
        o.setField(fix.Side(fix.Side_SELL))
        o.setField(fix.TransactTime())
        o.setField(fix.OrderQty(25))
        o.setField(fix.OrdType(fix.OrdType_LIMIT))
        o.setField(fix.Price(410.5))
        fix.Session.sendToTarget(o, app.sid)
    deadline = time.time() + 60
    while len(app.fills) < n and time.time() < deadline:
        time.sleep(0.05)
    secs = time.time() - t0
    ini.stop()
    ex.terminate()
    err = ex.communicate(timeout=10)[1]
    return {"orders_sent_by_quickfix": n, "fills_received_by_quickfix": len(app.fills), "quickfix_session_rejects_received": app.rejects,
            "seconds": secs, "fixwire_exchange_log_tail": err.strip().splitlines()[-1] if err.strip() else ""}


def scenario_c(build: Path, work: Path, port: int):
    app = Filler()
    s = settings("acceptor", port, "EXCH", "CLIENT", str(work / "qfc_store"), str(work / "qfc_log"))
    acc = fix.SocketAcceptor(app, fix.FileStoreFactory(s), s, fix.FileLogFactory(s))
    acc.start()
    time.sleep(0.5)
    store = work / "fw_store"
    r1 = run([str(build / "fixwire-client"), "--port", str(port), "--orders", "1000", "--window", "50", "--store", str(store), "--stop-after", "600", "--prefix", "C"])
    time.sleep(1.5)  # QuickFIX notices the dropped connection; fills for in-flight orders sit in its store
    r2 = run([str(build / "fixwire-client"), "--port", str(port), "--orders", "400", "--window", "50", "--store", str(store), "--start-index", "600", "--prefix", "C"])
    time.sleep(0.3)
    acc.stop()
    return {"run1_orders": r1["orders_sent"], "run1_fills": r1["fills_received"],
            "run2_recovered_fills_from_run1": r2["recovered_fills_from_previous_run"], "run2_orders": r2["orders_sent"],
            "run2_new_fills": r2["fills_received"] - r2["recovered_fills_from_previous_run"],
            "total_fills_for_1000_orders": r1["fills_received"] + r2["fills_received"],
            "quickfix_unique_orders": len(app.ids), "quickfix_orders_accepted": app.orders,
            "duplicate_fill_reports": r1["duplicate_fill_reports"] + r2["duplicate_fill_reports"],
            "resend_requests_sent_by_fixwire": r2["resend_requests_sent"], "possdup_duplicates_ignored": r2["duplicates_ignored"]}


def main():
    build, out = Path(sys.argv[1]).resolve(), sys.argv[2]
    work = Path(tempfile.mkdtemp(prefix="fixwire-interop-"))
    try:
        res = {"quickfix_version_dictionary": str(DICT.name),
               "A_fixwire_initiator_vs_quickfix_acceptor": scenario_a(build, work, 21001, 20000),
               "B_quickfix_initiator_vs_fixwire_acceptor": scenario_b(build, work, 21002, 5000),
               "C_crash_restart_resend_against_quickfix": scenario_c(build, work, 21003)}
    finally:
        shutil.rmtree(work, ignore_errors=True)
    Path(out).write_text(json.dumps(res, indent=1))
    print(json.dumps(res, indent=1))
    a, b, c = (res[k] for k in list(res)[1:])
    ok = (a["fills_received_by_fixwire"] == a["orders_sent"] == a["unique_ids"] and a["quickfix_session_rejects_received"] == 0
          and b["fills_received_by_quickfix"] == b["orders_sent_by_quickfix"] and b["quickfix_session_rejects_received"] == 0
          and c["total_fills_for_1000_orders"] == 1000 and c["quickfix_unique_orders"] == 1000 and c["duplicate_fill_reports"] == 0)
    print("INTEROP", "OK" if ok else "FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    os.environ.setdefault("PYTHONUNBUFFERED", "1")
    main()
