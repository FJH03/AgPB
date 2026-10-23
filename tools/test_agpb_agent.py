"""Offline parser/ownership/client checks; never construct a real console sender.
Run: py -B tools/test_agpb_agent.py. TEMP receipts are retained, not deleted.
"""
import contextlib
import copy
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("agpb_agent_under_test", Path(__file__).with_name("agpb_agent.py"))
client = importlib.util.module_from_spec(spec)
spec.loader.exec_module(client)


def snapshot(token=0, deadline=0.0, auto=1, alive=1, team=3, mapserial=2, userid=12):
    active = token != 0
    return ("[AgPB agent] v=1 map=de_dust2 mapserial=%d round=7 bots=1\n" % mapserial +
            "[AgPB] AgPB-1: agent v=1 userid=%d mapserial=%d round=7 lease-map=%d lease-round=%d "
            "token=%d mode=%s wp=%d deadline=%.3f remaining=3.000 state=%s reason=test auto=%d route-owner=%d\n" %
            (userid,mapserial,mapserial if active else 0,7 if active else 0,token,
             "camp" if active else "none",60 if active else -1,deadline,"active" if active else "none",auto,token) +
            "[AgPB lifecycle] v=1 name=AgPB-1 edict=2 userid=%d map=de_dust2 epoch=7 "
            "alive=%d lifestate=%d auto=%d team=%d desired=%d\n" % (userid,alive,0 if alive else 2,auto,team,team))


class Clock:
    def __init__(self): self.now = 0.0
    def monotonic(self): return self.now
    def sleep(self, seconds): self.now += seconds


class FakeSender:
    def __init__(self, clock, scenario="normal"):
        self.clock, self.scenario = clock, scenario
        self.process, self.commands, self.closed = object(), [], False
        self.token, self.deadline, self.reads = 0, 0.0, 0
    def send(self, command):
        assert self.frame_timeout > 0.75
        self.clock.now += 0.25
        self.commands.append(command)
        if command == client.READ_STATE:
            self.reads += 1
            auto = 0 if self.scenario == "stop" and self.token else 1
            serial = 3 if self.scenario == "scope" and self.token else 2
            token = 99 if self.scenario == "replacement" and self.token else self.token
            log = snapshot(token, self.deadline, auto=auto, mapserial=serial)
        else:
            if self.scenario == "error":
                raise client.CommandFailure("offline transport failure", {"command":command,"log":"failure"})
            assert command.startswith("agpb_agent_intent 2 7 12 camp 60 ")
            self.token, self.deadline = 42, 100 + self.clock.now + 3
            log = snapshot(self.token, self.deadline)
        return {"command":command,"log":log,"stdout":log,"offline_fake":True}
    def close(self): self.closed = True


class AgentClientTests(unittest.TestCase):
    def test_parser_identity_and_invalid_lifecycle(self):
        state = client.parse_state(snapshot(),12)
        self.assertEqual(state["identity"],("de_dust2",2,7,12,2,"AgPB-1",3,3))
        for text in (snapshot(auto=0),snapshot(alive=0),snapshot(userid=13),snapshot()+snapshot()):
            with self.assertRaises(ValueError): client.parse_state(text,12)

    def test_ownership_includes_deadline_goal_token_scope(self):
        owned = client.parse_state(snapshot(42,103.125),12)
        self.assertTrue(client.same_lease(copy.deepcopy(owned),owned))
        for key,value in (("token",43),("wp",61),("mode","move"),("mapserial",3),
                          ("round",8),("lease-map",3),("deadline",103.126)):
            changed = dict(owned); changed[key] = value
            self.assertFalse(client.same_lease(changed,owned),key)

    def run_client(self,scenario="normal",duration="3"):
        clock = Clock(); sender = FakeSender(clock,scenario); outputs = []
        real_mkdtemp = tempfile.mkdtemp
        def fresh(*args,**kwargs):
            result = real_mkdtemp(*args,**kwargs); outputs.append(Path(result)); return result
        argv = ["--run","--session-name","agpb_wp_offline","--session-directory",
                str(Path(tempfile.gettempdir())/"agpb_agent_offline_session"),"--userid","12",
                "--mode","camp","--waypoint","60","--ttl","3","--duration",duration,
                "--renew-interval","1","--powershell","offline-never-executed"]
        with patch.object(client,"ConsoleSender",return_value=sender), \
             patch.object(client.time,"monotonic",clock.monotonic), \
             patch.object(client.time,"sleep",clock.sleep), \
             patch.object(client.tempfile,"mkdtemp",fresh), contextlib.redirect_stdout(io.StringIO()):
            code = client.main(argv)
        summary = json.loads((outputs[0]/"summary.json").read_text(encoding="utf-8"))
        self.assertTrue(sender.closed)
        self.assertFalse(any("clear" in command for command in sender.commands))
        return code,sender,summary

    def test_normal_budget_end_success_and_same_token_renewal(self):
        code,sender,summary = self.run_client()
        self.assertEqual(code,0)
        self.assertEqual(summary["renewals"],2)
        self.assertEqual(summary["last_accepted_lease"]["token"],42)
        self.assertEqual(len(sender.commands),4)
        self.assertFalse(summary["automatic_clear"])

    def test_stop_scope_replacement_and_transport_error(self):
        for scenario,expected in (("stop",1),("scope",1),("replacement",0),("error",1)):
            code,sender,summary = self.run_client(scenario,duration="6")
            self.assertEqual(code,expected,scenario)
            intents = [command for command in sender.commands if command.startswith("agpb_agent_intent")]
            self.assertEqual(len(intents),1,scenario)


if __name__ == "__main__":
    unittest.main()
