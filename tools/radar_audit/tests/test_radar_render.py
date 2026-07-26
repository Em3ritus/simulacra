import os, subprocess, unittest

HERE = os.path.dirname(__file__); TOOL = os.path.dirname(HERE)
EXE = os.path.join(TOOL, "render_dump.exe" if os.name == "nt" else "render_dump")

# radar_view_t: HOME=0 RADAR=1 DETAIL=2 STATS=3 LIBRARY=4 CONTROL=5 INFO=6
STATS = 3
HOME = 0


def home(restless=1, wandering=1, bound=1, active=8, roster=16, target=8,
         threats=0, pop=10, esc=0, flags=0):
    """Render HOME and return its text lines. esc: 0 NEW, 1 RECURRING, 2 PERSISTENT."""
    out = subprocess.check_output(
        [EXE, str(HOME), str(restless), str(wandering), str(bound), str(active),
         str(roster), str(target), str(threats), str(pop), str(esc), str(flags)], text=True)
    return [ln.split(" ", 3)[3] for ln in out.splitlines() if ln.startswith("TXT ")]


def render(view, *args):
    """Return the list of text strings drawn by a view (the part after 'TXT x y ')."""
    out = subprocess.check_output([EXE, str(view), *[str(a) for a in args]], text=True)
    texts = []
    for ln in out.splitlines():
        if ln.startswith("TXT "):
            texts.append(ln.split(" ", 3)[3])   # drop 'TXT', x, y
    return texts


def stats(restless=0, wandering=0, bound=0, active=8, roster=16, target=8, uptime=0):
    out = subprocess.check_output(
        [EXE, str(STATS), str(restless), str(wandering), str(bound), str(active),
         str(roster), str(target), "0", "0", "0", "0", str(uptime)], text=True)
    return [ln.split(" ", 3)[3] for ln in out.splitlines() if ln.startswith("TXT ")]


DETAIL = 2


def detail(threats=0, ncam=0, esc=0):
    # positional: view, restless,wandering,bound, active,roster,target, threats, pop, esc, flags, uptime, ncam
    return render(DETAIL, 1, 1, 1, 8, 16, 8, threats, 10, esc, 0, 0, ncam)


def home_surv(threats=0, ncam=0):
    return render(HOME, 1, 1, 1, 8, 16, 8, threats, 10, 0, 0, 0, ncam)


@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class SurveillancePresence(unittest.TestCase):
    def test_camera_shows_surveillance_section(self):
        texts = detail(threats=1, ncam=1)
        self.assertIn("SURVEILLANCE", texts, f"no surveillance section; drew: {texts}")
        self.assertIn("Flock", texts, f"no Flock label; drew: {texts}")

    def test_camera_excluded_from_follower_count(self):
        # 3 threats, 1 camera -> the follower summary counts only the 2 non-cameras
        texts = detail(threats=3, ncam=1)
        self.assertTrue(any("2 seen" in t for t in texts), f"follower count wrong; drew: {texts}")
        self.assertIn("SURVEILLANCE", texts)

    def test_followers_only_no_surveillance(self):
        texts = detail(threats=2, ncam=0)
        self.assertNotIn("SURVEILLANCE", texts, f"unexpected surveillance section; drew: {texts}")
        self.assertTrue(any("2 seen" in t for t in texts))

    def test_home_surveil_indicator_present(self):
        texts = home_surv(threats=1, ncam=1)
        self.assertIn("!1", texts, f"no HOME surveil indicator; drew: {texts}")

    def test_home_no_indicator_without_camera(self):
        texts = home_surv(threats=1, ncam=0)
        self.assertFalse(any(t.startswith("!") for t in texts), f"unexpected indicator; drew: {texts}")


@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class SurveillanceBodycam(unittest.TestCase):
    def test_bodycam_shows_surveillance_and_axon_label(self):
        # DETAIL: 1 threat, 1 camera, surv_kind=1 (Axon/BODYCAM)
        texts = render(DETAIL, 1, 1, 1, 8, 16, 8, 1, 10, 0, 0, 0, 1, 1)
        self.assertIn("SURVEILLANCE", texts, f"no surveillance section; drew: {texts}")
        self.assertIn("Axon", texts, f"no Axon label; drew: {texts}")

    def test_home_indicator_for_bodycam(self):
        texts = render(HOME, 1, 1, 1, 8, 16, 8, 1, 10, 0, 0, 0, 1, 1)
        self.assertIn("!1", texts, f"no HOME surveil indicator for bodycam; drew: {texts}")


@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class StatsFormBreakdown(unittest.TestCase):
    def test_stats_shows_form_breakdown_values(self):
        # restless=5 wandering=3 bound=7 -> the DECOYS page surfaces the RPA/NRPA/static split as an
        # aligned label + value row.
        texts = stats(5, 3, 7)
        self.assertTrue(any("rpa/nrpa/static" in t for t in texts), f"no form label; drew: {texts}")
        self.assertIn("5 / 3 / 7", texts, f"form values row missing; drew: {texts}")

    def test_stats_grouped_into_sections(self):
        texts = stats()
        for section in ("DECOY CROWD", "ENVIRONMENT", "SYSTEM"):
            self.assertIn(section, texts, f"missing section {section!r}; drew: {texts}")

    def test_stats_uptime_humanized(self):
        # 47143s -> "13h 5m", not raw seconds
        self.assertIn("13h 5m", stats(uptime=47143), "uptime not humanized")

    def test_stats_still_shows_existing_rows(self):
        # regression: the page still identifies as the DECOYS view.
        joined = " ".join(stats(5, 3, 8, active=12, roster=16, target=10)).lower()
        self.assertIn("decoy", joined, "STATS lost its DECOYS identity")

    def test_data_views_share_back_and_title_header(self):
        # STATS/DETAIL/LIBRARY/INFO get the shared themed header: a "< BACK" affordance + page title,
        # matching HOME/CONTROL. (threat_count=0 so DETAIL renders its header path.)
        for view, title in [(2, "FOLLOWERS"), (3, "DECOYS"), (4, "LIBRARY"), (6, "INFO")]:
            texts = render(view, 1, 1, 1, 4, 8, 4, 0)
            joined = " | ".join(texts)
            self.assertIn("< BACK", texts, f"view {view} missing BACK affordance; drew: {joined}")
            self.assertTrue(any(title in t for t in texts),
                            f"view {view} missing title {title!r}; drew: {joined}")

    def test_all_views_render_without_crash(self):
        # smoke: every view renders (harness exits 0) for a representative populated status.
        for v in range(0, 7):
            subprocess.check_call([EXE, str(v), "5", "3", "8", "12", "16", "10", "2"],
                                  stdout=subprocess.DEVNULL)


def view_texts(view, uptime=0):
    out = subprocess.check_output(
        [EXE, str(view), "0", "0", "0", "8", "16", "8", "0", "0", "0", "0", str(uptime)], text=True)
    return [ln.split(" ", 3)[3] for ln in out.splitlines() if ln.startswith("TXT ")]


def followers(threats=0, esc=0):
    out = subprocess.check_output(
        [EXE, "2", "0", "0", "0", "8", "16", "8", str(threats), "0", str(esc), "0"], text=True)
    return [ln.split(" ", 3)[3] for ln in out.splitlines() if ln.startswith("TXT ")]


@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class FollowersList(unittest.TestCase):
    def test_empty_says_none(self):
        self.assertTrue(any("none" in t.lower() for t in followers(0)))

    def test_summary_counts_flagged(self):
        t = followers(threats=3, esc=1)   # 3 recurring -> all flagged
        self.assertTrue(any("3 seen" in x for x in t), f"no summary; drew: {t}")
        self.assertTrue(any("3 flagged" in x for x in t), f"flagged count wrong; drew: {t}")

    def test_new_threats_not_flagged_and_labeled_new(self):
        t = followers(threats=2, esc=0)   # NEW -> 0 flagged, rows read "new"
        self.assertTrue(any("0 flagged" in x for x in t), f"NEW should be 0 flagged; drew: {t}")
        self.assertTrue(any(x == "new" for x in t), f"NEW row not labeled; drew: {t}")

    def test_one_row_per_threat(self):
        t = followers(threats=4, esc=1)   # hash 0 -> name "00000000"
        self.assertGreaterEqual(sum(1 for x in t if x == "00000000"), 4, f"missing rows; drew: {t}")


@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class DataPageLayout(unittest.TestCase):
    def test_library_grouped_into_sections(self):
        t = view_texts(4)   # LIBRARY
        for s in ("LIBRARY", "STORAGE", "SYNC"):
            self.assertIn(s, t, f"LIBRARY missing {s!r}; drew: {t}")
        self.assertTrue(any("card" in x for x in t), f"no card row; drew: {t}")

    def test_info_grouped_and_uptime_humanized(self):
        t = view_texts(6, uptime=47143)   # INFO
        self.assertIn("SYSTEM", t, f"INFO missing SYSTEM; drew: {t}")
        self.assertTrue(any("firmware" in x for x in t), f"no firmware row; drew: {t}")
        self.assertIn("13h 5m", t, f"INFO uptime not humanized; drew: {t}")


@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class ProtectionPosture(unittest.TestCase):
    def test_cloaked_when_active_with_crowd(self):
        self.assertIn("CLOAKED", home(active=8, pop=12, threats=0))

    def test_status_label_present(self):
        # the posture word is labeled "STATUS" so it reads as the system's current status
        self.assertIn("STATUS", home(active=8, pop=12, threats=0))

    def test_exposed_when_no_crowd(self):
        # decoys running but essentially no ambient devices -> honest "nothing hides you"
        self.assertIn("EXPOSED", home(active=8, pop=0, threats=0))

    def test_dark_when_paused(self):
        self.assertIn("DARK", home(active=8, pop=12, threats=0, flags=1))

    def test_dark_when_not_emitting(self):
        self.assertIn("DARK", home(active=0, pop=12, threats=0))

    def test_hunted_on_confirmed_follower(self):
        self.assertIn("HUNTED", home(active=8, pop=12, threats=1, esc=1))   # RECURRING
        self.assertIn("HUNTED", home(active=8, pop=12, threats=1, esc=2))   # PERSISTENT

    def test_new_threat_does_not_cry_wolf(self):
        # an unconfirmed (NEW) threat must NOT read HUNTED -- it stays in FOLLOWERS only.
        lines = home(active=8, pop=12, threats=1, esc=0)
        self.assertNotIn("HUNTED", lines)
        self.assertIn("CLOAKED", lines)

    def test_hunted_beats_paused(self):
        # priority: a confirmed follower wins even if the fleet is paused.
        self.assertIn("HUNTED", home(active=8, pop=12, threats=1, esc=2, flags=1))

    def test_exactly_one_posture_word(self):
        words = {"CLOAKED", "EXPOSED", "DARK", "HUNTED"}
        for kw in (dict(pop=12), dict(pop=0), dict(flags=1), dict(threats=1, esc=2)):
            lines = home(active=8, **kw)
            self.assertEqual(sum(1 for t in lines if t in words), 1, f"not exactly one posture: {lines}")


def exposure(step, fp=0xbb, probes=5, ssids="HomeWiFi,CoffeeShop", ambiguous=0):
    # render_dump --expo <step 0=idle 1=baseline 2=watch 3=result> <fp> <probes> <ambiguous> <ssids csv>
    out = subprocess.check_output([EXE, "--expo", str(step), hex(fp), str(probes),
                                   str(ambiguous), ssids], text=True)
    return [ln.split(" ", 3)[3] for ln in out.splitlines() if ln.startswith("TXT ")]


@unittest.skipUnless(os.path.exists(EXE), "render_dump not built")
class ExposureView(unittest.TestCase):
    def test_idle_prompts_scan(self):
        self.assertTrue(any("SCAN" in t.upper() for t in exposure(0)))

    def test_watch_prompts_toggle(self):
        t = exposure(2)
        self.assertTrue(any("TOGGLE" in x.upper() or "WI-FI" in x.upper() for x in t))

    def test_result_shows_leaked_ssids(self):
        t = exposure(3, ssids="HomeWiFi,CoffeeShop")
        self.assertTrue(any("HomeWiFi" in x for x in t))
        self.assertTrue(any("CoffeeShop" in x for x in t))

    def test_ambiguous_result_says_so(self):
        t = exposure(3, ambiguous=1)
        self.assertTrue(any("AGAIN" in x.upper() or "NO " in x.upper() for x in t))


if __name__ == "__main__":
    unittest.main()
