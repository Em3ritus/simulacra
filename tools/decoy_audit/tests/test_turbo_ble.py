import os, subprocess, unittest
HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "synth_dump.exe" if os.name == "nt" else "synth_dump")


def devices(seed=1, n=8, ticks=20, tick_ms=1000, turbo=False):
    args = [EXE, "--devices", str(seed), str(n), str(ticks), str(tick_ms)]
    if turbo:
        args.append("turbo")
    out = subprocess.check_output(args, text=True)
    return [ln.split() for ln in out.splitlines() if ln.startswith("D ")]
    # columns: [0]="D" [1]=t [2]=slot [3]=addr_hex [4]=atype [5]=role [6]=event [7]=company [8]=itvl


def devices_lateturbo(seed=1, n=8, pre_ticks=5, post_ticks=20, tick_ms=1000):
    args = [EXE, "--devices-lateturbo", str(seed), str(n), str(pre_ticks), str(post_ticks), str(tick_ms)]
    out = subprocess.check_output(args, text=True)
    # columns: [0]="D" [1]=t [2]=slot [3]="born"
    return [ln.split() for ln in out.splitlines() if ln.startswith("D ")]


@unittest.skipUnless(os.path.exists(EXE), "synth_dump not built")
class TurboBleChurn(unittest.TestCase):
    def test_turbo_respawns_far_more_than_normal(self):
        # 20 x 1s ticks is far shorter than any normal lifetime band (shortest is the 2 min
        # transient minimum), so normal mode should show only the initial births in this window.
        normal = devices(seed=3, n=8, ticks=20, tick_ms=1000, turbo=False)
        turbo = devices(seed=3, n=8, ticks=20, tick_ms=1000, turbo=True)
        normal_born = [r for r in normal if r[6] == "born"]
        turbo_born = [r for r in turbo if r[6] == "born"]
        self.assertGreater(len(turbo_born), len(normal_born),
                           f"turbo should respawn far more: normal={len(normal_born)} turbo={len(turbo_born)}")

    def test_turbo_life_is_short(self):
        # Every "born" event past t=0 on a given slot is a respawn. Gaps between successive births
        # on the same slot must sit inside the turbo band (2-5s) with slack for tick granularity.
        rows = devices(seed=5, n=4, ticks=15, tick_ms=1000, turbo=True)
        by_slot = {}
        for r in rows:
            if r[6] != "born":
                continue
            slot = int(r[2]); t = int(r[1])
            by_slot.setdefault(slot, []).append(t)
        gaps = [b - a for times in by_slot.values() for a, b in zip(times, times[1:])]
        self.assertTrue(gaps, "no respawns observed in turbo mode")
        for g in gaps:
            self.assertLessEqual(g, 6000, f"turbo respawn gap too slow: {g} ms")

    def test_turbo_still_varies_atype_and_company(self):
        # Turbo only shortens life_ms; the atype/company/payload draw is untouched, so respawns
        # must still show more than one distinct atype and more than one distinct company over a
        # long-enough run -- a wiring bug that collapsed everything to one shape would fail this.
        rows = devices(seed=7, n=8, ticks=30, tick_ms=1000, turbo=True)
        atypes = {r[4] for r in rows if r[6] == "born"}
        companies = {r[7] for r in rows if r[6] == "born"}
        self.assertGreater(len(atypes), 1, f"turbo collapsed to one atype: {atypes}")
        self.assertGreater(len(companies), 1, f"turbo collapsed to one company: {companies}")

    def test_turbo_switched_on_midrun_respawns_preexisting_slots(self):
        # I-1 regression guard: init with turbo OFF (devices land on normal 30 min-12 h bands, so
        # none would ever respawn on their own inside this whole window), run a few ticks so the
        # crowd is genuinely alive on the slow bands, THEN flip turbo on and keep ticking. Every
        # slot that existed before the switch must produce a fresh identity within ~6 s of the
        # switch -- not just newly-spawned slots (there are none here; growth doesn't apply).
        rows = devices_lateturbo(seed=9, n=8, pre_ticks=5, post_ticks=20, tick_ms=1000)
        # pre_ticks=5 ticks x 1000ms = turbo enabled at t=5000; the switch-time snapshot itself is
        # never printed (memcmp against prev only fires on an actual address change), so every row
        # here is a genuine post-switch respawn.
        by_slot = {}
        for r in rows:
            slot = int(r[2]); t = int(r[1])
            by_slot.setdefault(slot, []).append(t)
        self.assertEqual(len(by_slot), 8,
                          f"expected all 8 pre-existing slots to respawn after turbo switched on, got {by_slot}")
        for slot, times in by_slot.items():
            first = times[0]
            self.assertLessEqual(first, 5000 + 6000,
                                  f"slot {slot} took too long to respawn after turbo-on: first={first}")


if __name__ == "__main__":
    unittest.main()
