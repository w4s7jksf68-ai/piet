// SPDX-License-Identifier: GPL-3.0-or-later
// Existing behavioral tests: calendars.
#include "selftest_support.h"
#include "selftest_cases.h"


// ── AER scope-1 (2026-09-03): the two campaign ratchets ──────────────────────
//
// The AER scope-1 campaign wires ~145 new commands through one 14-step
// checklist. Two of those steps had no gate of their own, so they are the two
// that silently got skipped before: commit 5a49d52d added xtile/ipolate/tssmooth
// with no floors.json classification and no effect-table row, and the floor debt
// only surfaced a month later as a structural RED nobody had touched. A hundred
// more rows would have multiplied that silence. These two checks make the
// omission fail at the moment it lands, which is the only moment it is cheap.
//
//   (1) EFFECT-TABLE COVERAGE. Every registry row is either classified in
//       parallel_effects.cpp's table or named on the frozen pre-campaign
//       exemption list. Absence is SAFE at runtime — parallel_effects.h's
//       axiom makes an unclassified head refuse the whole auto-parallel region
//       rather than guess — but it is a silent loss of auto eligibility, and
//       silence is exactly what a campaign of this size cannot afford.
//       The exemption list is FROZEN at the 70 rows that were unclassified on
//       2026-09-03. It may SHRINK, never grow: a new command classifies, or it
//       is not finished.
//
//   (2) NO-DATA ROUTES. A command that must run with no dataset loaded
//       actually does. This is the BUG-343 defect class: a correct registry
//       row and a correct dispatcher arm still answered `No dataset is
//       loaded.` because dispatcher.cpp's kNoDataCommands set never learned
//       the name — the failure is invisible to the route drift guard above,
//       which loads a dataset before every probe. Probed behaviourally (the
//       set itself is a static local), so the test needs no new export.
// AER scope-2 bundle B2 -- oaxaca. Every numeric pin is a StataBE 19.5 +
// oaxaca 4.1.1 %21x value from tests/validate/b2_oracle.json, captured on the
// in-body fixture below BEFORE the kernel was written. The five SE formulas
// the first pass guessed were all wrong by >= 5.5e-4 on group_1; the pins
// here are at 1e-9, which none of the five could pass.
// AER scope-2 bundle B2-3: `bcal create` + bofd()/dofb() (paper 066).
//
// THE PIN IS THE FILE'S BYTES. Every kXxx literal below is a `.stbcal` file
// StataBE 19.5 wrote, `type`d verbatim into internal capture
// capture*.log / refute0.log between begin/end TYPE markers -- not a file re-read
// from that directory (which holds each calendar only as LAST written), and
// not a shape this build produced and was then blessed with.
//
// Two legs are the ones the receipt's first, INFERRED rule got wrong ("emit an
// `omit date` for every calendar day in the range that is not one of the
// values"): k_nonint and z_ni, both with non-integer from() values. They come
// out right only under Stata's consecutive-difference rule. z_ni has an
// 11.5-day RAW gap and still passes maxgap(10), emitting no omit-date line at
// all -- REFUTE-0 T5's discriminating shape, and the reason this file is here.
void testAerB2Bcal() {
  std::printf("[selftest] --- AER scope-2 bundle B2-3: bcal / bofd / dofb ---\n");
  namespace fs = std::filesystem;
  const std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_aerb2bcal").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  static const char* kAWk =
      "* Business calendar \"a_wk\" created by -bcal create-\n"
      "* Created/replaced on  8 Sep 2026\n"
      "\n"
      "version 19.5\n"
      "dateformat ymd\n"
      "\n"
      "range 2020jan01 2020feb09\n"
      "centerdate 2020jan01\n"
      "\n"
      "omit dayofweek (Mo Tu)\n";

  static const char* kBHol =
      "* Business calendar \"b_hol\" created by -bcal create-\n"
      "* Created/replaced on  8 Sep 2026\n"
      "\n"
      "version 19.5\n"
      "dateformat ymd\n"
      "\n"
      "range 2020jan01 2020feb28\n"
      "centerdate 2020jan01\n"
      "\n"
      "omit dayofweek (Sa Su)\n"
      "omit date 2020jan03\n"
      "omit date 2020jan20\n"
      "omit date 2020feb12\n"
      "omit date 2020feb13\n"
      "omit date 2020feb17\n";

  static const char* kBDaily =
      "* Business calendar \"b_daily\" created by -bcal create-\n"
      "* Created/replaced on  8 Sep 2026\n"
      "\n"
      "version 19.5\n"
      "dateformat ymd\n"
      "\n"
      "range 2020jan01 2020jan31\n"
      "centerdate 2020jan01\n"
      "\n"
      "omit date 2020jan05\n"
      "omit date 2020jan15\n"
      "omit date 2020jan16\n";

  static const char* kCG11 =
      "* Business calendar \"c_g11\" created by -bcal create-\n"
      "* Created/replaced on  8 Sep 2026\n"
      "\n"
      "version 19.5\n"
      "dateformat ymd\n"
      "\n"
      "range 2020jan01 2020jan31\n"
      "centerdate 2020jan01\n"
      "\n"
      "omit date 2020jan10\n"
      "omit date 2020jan11\n"
      "omit date 2020jan12\n"
      "omit date 2020jan13\n"
      "omit date 2020jan14\n"
      "omit date 2020jan15\n"
      "omit date 2020jan16\n"
      "omit date 2020jan17\n"
      "omit date 2020jan18\n"
      "omit date 2020jan19\n";

  static const char* kCG12 =
      "* Business calendar \"c_g12\" created by -bcal create-\n"
      "* Created/replaced on  8 Sep 2026\n"
      "\n"
      "version 19.5\n"
      "dateformat ymd\n"
      "\n"
      "range 2020jan01 2020jan31\n"
      "centerdate 2020jan01\n"
      "\n"
      "omit date 2020jan10\n"
      "omit date 2020jan11\n"
      "omit date 2020jan12\n"
      "omit date 2020jan13\n"
      "omit date 2020jan14\n"
      "omit date 2020jan15\n"
      "omit date 2020jan16\n"
      "omit date 2020jan17\n"
      "omit date 2020jan18\n"
      "omit date 2020jan19\n"
      "omit date 2020jan20\n";

  static const char* kDSrt =
      "* Business calendar \"d_srt\" created by -bcal create-\n"
      "* Created/replaced on  8 Sep 2026\n"
      "\n"
      "version 19.5\n"
      "dateformat ymd\n"
      "\n"
      "range 2020jan01 2020feb28\n"
      "centerdate 2020jan01\n"
      "\n"
      "omit dayofweek (Sa Su)\n"
      "omit date 2020jan03\n"
      "omit date 2020jan20\n"
      "omit date 2020feb12\n"
      "omit date 2020feb13\n"
      "omit date 2020feb17\n";

  static const char* kGCtr =
      "* Business calendar \"g_ctr\" created by -bcal create-\n"
      "* Created/replaced on  8 Sep 2026\n"
      "\n"
      "version 19.5\n"
      "dateformat ymd\n"
      "\n"
      "range 2020jan06 2020feb07\n"
      "centerdate 2020jan15\n"
      "\n"
      "omit dayofweek (Sa Su)\n";

  static const char* kGLead =
      "* Business calendar \"g_lead\" created by -bcal create-\n"
      "* Created/replaced on  8 Sep 2026\n"
      "\n"
      "version 19.5\n"
      "purpose \"Demo purpose text\"\n"
      "dateformat dmy\n"
      "\n"
      "range 06jan2020 07feb2020\n"
      "centerdate 06jan2020\n"
      "\n"
      "omit dayofweek (Sa Su)\n";

  static const char* kKNonint =
      "* Business calendar \"k_nonint\" created by -bcal create-\n"
      "* Created/replaced on  8 Sep 2026\n"
      "\n"
      "version 19.5\n"
      "dateformat ymd\n"
      "\n"
      "range 2020jan01 2020jan05\n"
      "centerdate 2020jan01\n"
      "\n"
      "omit dayofweek (Mo Tu Fr)\n"
      "omit date 2020jan02\n";

  static const char* kRLead =
      "* Business calendar \"r_lead\" created by -bcal create-\n"
      "* Created/replaced on  8 Sep 2026\n"
      "\n"
      "version 19.5\n"
      "dateformat ymd\n"
      "\n"
      "range 2020jan01 2020feb12\n"
      "centerdate 2020jan01\n"
      "\n"
      "omit dayofweek (Sa Su)\n"
      "omit date 2020jan02\n"
      "omit date 2020jan03\n"
      "omit date 2020feb10\n"
      "omit date 2020feb11\n";

  static const char* kZNi =
      "* Business calendar \"z_ni\" created by -bcal create-\n"
      "* Created/replaced on  8 Sep 2026\n"
      "\n"
      "version 19.5\n"
      "dateformat ymd\n"
      "\n"
      "range 2020jan01 2020jan15\n"
      "centerdate 2020jan01\n"
      "\n"
      "omit dayofweek (Mo Tu Su)\n";

  static const char* kESp =
      "* Business calendar \"e_sp\" created by -bcal create-\n"
      "* Created/replaced on  8 Sep 2026\n"
      "\n"
      "version 19.5\n"
      "dateformat ymd\n"
      "\n"
      "range 2020jan02 2021dec31\n"
      "centerdate 2020jan02\n"
      "\n"
      "omit dayofweek (Sa Su)\n"
      "omit date 2020jan20\n"
      "omit date 2020feb17\n"
      "omit date 2020apr10\n"
      "omit date 2020may25\n"
      "omit date 2020jul03\n"
      "omit date 2020sep07\n"
      "omit date 2020nov26\n"
      "omit date 2020dec25\n"
      "omit date 2021jan01\n"
      "omit date 2021jan18\n"
      "omit date 2021feb15\n"
      "omit date 2021apr02\n"
      "omit date 2021may31\n"
      "omit date 2021jul05\n"
      "omit date 2021sep06\n"
      "omit date 2021nov25\n"
      "omit date 2021dec24\n";


  const std::string kToday = "* Created/replaced on  8 Sep 2026";
  auto td = [](int y, unsigned m, unsigned d) {
    return static_cast<double>(piet::bcal::daysFromCivil(y, m, d));
  };
  // `WK n` and the daily fixture are the capture do-files' own programs.
  auto wk = [&](int n) {
    std::vector<double> v;
    for (int i = 0; i < n; ++i) {
      const double d = td(2020, 1, 1) + i;
      const int w = piet::bcal::dowOf(static_cast<std::int64_t>(d));
      if (w != 0 && w != 6) v.push_back(d);
    }
    return v;
  };
  auto daily = [&](int n) {
    std::vector<double> v;
    for (int i = 0; i < n; ++i) v.push_back(td(2020, 1, 1) + i);
    return v;
  };
  auto without = [](std::vector<double> v, const std::vector<double>& drop) {
    std::vector<double> o;
    for (double x : v) {
      bool skip = false;
      for (double d : drop)
        if (x == d) skip = true;
      if (!skip) o.push_back(x);
    }
    return o;
  };
  auto induce = [&](const std::string& name, const std::vector<double>& v,
                    piet::bcal::InduceOpts o) {
    o.calName = name;
    o.todayLine = kToday;
    return piet::bcal::induceFromDates(v, o);
  };
  auto byteCheck = [&](const std::string& name, const std::vector<double>& v,
                       const piet::bcal::InduceOpts& o, const char* want) {
    piet::bcal::InduceResult r = induce(name, v, o);
    check(r.ok && r.text == want,
          "B2-3 bcal write: " + name + ".stbcal byte-identical to StataBE's own file" +
              (r.ok ? (r.text == want ? std::string() : " -- GOT:\n" + r.text)
                    : " -- rc " + std::to_string(r.rc) + " " + r.message));
  };

  const piet::bcal::InduceOpts kPlain;

  // --- 1..3: the plain shapes ---------------------------------------------
  {  // (a) 40 days from 01jan2020 minus Monday and Tuesday
    std::vector<double> v;
    for (double d : daily(40)) {
      const int w = piet::bcal::dowOf(static_cast<std::int64_t>(d));
      if (w != 1 && w != 2) v.push_back(d);
    }
    byteCheck("a_wk", v, kPlain, kAWk);
  }
  byteCheck("b_hol",
            without(wk(59), {td(2020, 1, 3), td(2020, 1, 20), td(2020, 2, 12), td(2020, 2, 13),
                             td(2020, 2, 17)}),
            kPlain, kBHol);
  // A DAILY calendar missing three days emits `omit date` lines and NO
  // `omit dayofweek` line at all -- one Sunday absent out of four does not
  // make Sunday an omitted weekday (the census is "occurs in NO value").
  byteCheck("b_daily", without(daily(31), {td(2020, 1, 5), td(2020, 1, 15), td(2020, 1, 16)}),
            kPlain, kBDaily);

  // --- 4..6: the maxgap boundary ------------------------------------------
  {
    std::vector<double> hole10, hole11;
    for (double d : daily(31)) {
      if (d < td(2020, 1, 10) || d > td(2020, 1, 19)) hole10.push_back(d);
      if (d < td(2020, 1, 10) || d > td(2020, 1, 20)) hole11.push_back(d);
    }
    byteCheck("c_g11", hole10, kPlain, kCG11);  // 10 omitted days: exactly maxgap
    piet::bcal::InduceResult bad = induce("c_g12", hole11, kPlain);
    check(!bad.ok && bad.rc == 198,
          "B2-3 bcal maxgap: an 11-day hole is rc 198 under the default maxgap(10) (oracle "
          "c12_rc), got rc " + std::to_string(bad.rc));
    piet::bcal::InduceOpts mg11;
    mg11.maxgap = 11;
    byteCheck("c_g12", hole11, mg11, kCG12);
    piet::bcal::InduceOpts mg0;
    mg0.maxgap = 0;
    piet::bcal::InduceResult z = induce("c_g12", hole11, mg0);
    check(!z.ok && z.rc == 198,
          "B2-3 bcal maxgap: maxgap(0) is rc 198 `should be at least 1' (oracle c_mg0_rc)");
  }

  // --- 7: order and duplicates are irrelevant ------------------------------
  {
    std::vector<double> v = without(wk(59), {td(2020, 1, 3), td(2020, 1, 20), td(2020, 2, 12),
                                             td(2020, 2, 13), td(2020, 2, 17)});
    std::vector<double> shuffled;
    for (size_t i = 0; i < v.size(); ++i) shuffled.push_back(v[(i * 7 + 3) % v.size()]);
    for (size_t i = 0; i < v.size(); i += 4) shuffled.push_back(v[i]);  // duplicates
    byteCheck("d_srt", shuffled, kPlain, kDSrt);
  }

  // --- 8..10: centerdate, dateformat/purpose, range() ---------------------
  {
    std::vector<double> g;
    for (double d : wk(40))
      if (d >= td(2020, 1, 6)) g.push_back(d);
    piet::bcal::InduceOpts ctr;
    ctr.haveCenter = true;
    ctr.center = static_cast<std::int64_t>(td(2020, 1, 15));
    byteCheck("g_ctr", g, ctr, kGCtr);

    piet::bcal::InduceOpts dmy;
    dmy.dateformat = "dmy";
    dmy.purpose = "Demo purpose text";
    byteCheck("g_lead", g, dmy, kGLead);

    // range() on BOTH sides: the endpoints become business days even though
    // they are not in the data, and the days between them and the data are
    // emitted as `omit date` unless their weekday is already omitted.
    piet::bcal::InduceOpts rng;
    rng.haveRangeLo = rng.haveRangeHi = true;
    rng.rangeLo = static_cast<std::int64_t>(td(2020, 1, 1));
    rng.rangeHi = static_cast<std::int64_t>(td(2020, 2, 12));
    byteCheck("r_lead", g, rng, kRLead);
  }

  // --- 11..12: the two non-integer shapes ---------------------------------
  byteCheck("k_nonint", {21915.0, 21916.5, 21918.0, 21919.0}, kPlain, kKNonint);
  byteCheck("z_ni", {21915.0, 21916.0, 21917.0, 21918.0, 21929.5}, kPlain, kZNi);

  // --- 13: the S&P-shaped 505-day calendar --------------------------------
  {
    std::vector<double> e;
    const std::vector<double> hol = {
        td(2020, 1, 1),  td(2020, 1, 20), td(2020, 2, 17), td(2020, 4, 10), td(2020, 5, 25),
        td(2020, 7, 3),  td(2020, 9, 7),  td(2020, 11, 26), td(2020, 12, 25), td(2021, 1, 1),
        td(2021, 1, 18), td(2021, 2, 15), td(2021, 4, 2),  td(2021, 5, 31), td(2021, 7, 5),
        td(2021, 9, 6),  td(2021, 11, 25), td(2021, 12, 24)};
    for (int i = 0; i < 731; ++i) {
      const double d = td(2020, 1, 1) + i;
      const int w = piet::bcal::dowOf(static_cast<std::int64_t>(d));
      if (w == 0 || w == 6) continue;
      bool isHol = false;
      for (double h : hol)
        if (h == d) isHol = true;
      if (!isHol) e.push_back(d);
    }
    byteCheck("e_sp", e, kPlain, kESp);
  }

  // --- 14..18: the reader, and the index arithmetic on it ------------------
  {
    piet::bcal::ParseResult p = piet::bcal::parseStbcal(kAWk, "a_wk");
    check(p.ok, "B2-3 bcal read: the file bcal create wrote loads back (the self-describe step)");
    // capture.log's a_bofd_* sweep over 30dec2019..11feb2020, spot-checked at
    // every boundary it contains.
    check(std::isnan(piet::bcal::bofd(p.cal, td(2019, 12, 30))),
          "B2-3 bofd: a date BEFORE the range is missing (a_bofd_21913)");
    check(piet::bcal::bofd(p.cal, td(2020, 1, 1)) == 0,
          "B2-3 bofd: the range start is index 0 (a_bofd_21915)");
    check(std::isnan(piet::bcal::bofd(p.cal, td(2020, 1, 6))),
          "B2-3 bofd: an OMITTED weekday is missing, never the previous business day "
          "(a_bofd_21920, arm A8)");
    check(piet::bcal::bofd(p.cal, td(2020, 2, 9)) == 29 &&
              std::isnan(piet::bcal::bofd(p.cal, td(2020, 2, 11))),
          "B2-3 bofd: the range end is the last index (a_r_max_date_tb = 29), past it is "
          "missing");
    // Truncation is toward ZERO, and the fractional part never moves the answer.
    check(piet::bcal::bofd(p.cal, 21916.5) == 1 && piet::bcal::bofd(p.cal, 21916.0001) == 1 &&
              piet::bcal::bofd(p.cal, 21915.9999) == 0,
          "B2-3 bofd: trunc toward zero (a_bofd_halfday/_eps/_negeps)");
    check(std::isnan(piet::bcal::bofd(p.cal, std::nan(""))),
          "B2-3 bofd: a missing date is missing");
    check(piet::bcal::dofb(p.cal, 2.5) == 21917 && piet::bcal::dofb(p.cal, 2.0001) == 21917,
          "B2-3 dofb: trunc toward zero (a_dofb_half/_eps)");
    check(std::isnan(piet::bcal::dofb(p.cal, -1)) && std::isnan(piet::bcal::dofb(p.cal, 30)) &&
              std::isnan(piet::bcal::dofb(p.cal, 1e10)),
          "B2-3 dofb: out of range is MISSING, never clamped (arm A9)");
    // Round trip over every business day.
    bool rt = true;
    for (std::int64_t i = p.cal.minTb; i <= p.cal.maxTb; ++i) {
      const double d = piet::bcal::dofb(p.cal, static_cast<double>(i));
      if (std::isnan(d) || piet::bcal::bofd(p.cal, d) != static_cast<double>(i)) rt = false;
    }
    check(rt, "B2-3 round trip: dofb(bofd(d)) == d on every business day (a_rt_mismatch = 0)");
  }
  {  // negative indices before an explicit centerdate (g_ctr, centerdate 15jan)
    piet::bcal::ParseResult p = piet::bcal::parseStbcal(kGCtr, "g_ctr");
    check(p.ok && piet::bcal::bofd(p.cal, td(2020, 1, 6)) == -7 &&
              piet::bcal::bofd(p.cal, td(2020, 1, 14)) == -1 &&
              piet::bcal::bofd(p.cal, td(2020, 1, 15)) == 0,
          "B2-3 bofd: indices are NEGATIVE before the centerdate (g_c15_bofd_06jan/_14jan)");
    check(piet::bcal::dofb(p.cal, -7) == td(2020, 1, 6) && std::isnan(piet::bcal::dofb(p.cal, -8)),
          "B2-3 dofb: -7 is 06jan2020 and -8 is missing (g_c15_dofb_neg7/_neg8)");
  }
  {  // z_ni: 9 business days and bofd(15jan2020) == 8 (REFUTE-0 T5's numbers)
    piet::bcal::ParseResult p = piet::bcal::parseStbcal(kZNi, "z_ni");
    check(p.ok && p.cal.maxTb - p.cal.minTb + 1 == 9 &&
              piet::bcal::bofd(p.cal, td(2020, 1, 15)) == 8,
          "B2-3 bcal: z_ni has r(included) 9 and bofd(15jan2020) 8 -- the diff-walk's answer, "
          "not the presence census's");
  }
  {  // the reader's own rejections, each one a measured rc 197
    struct R {
      const char* text;
      const char* what;
    };
    const std::vector<R> bad = {
        {"version 19.5\ndateformat ymd\nrange 2020jan01 2020feb07\ncenterdate 2020jan01\n"
         "omit dayofweek (Sa Su)\nomit dayofweek We if month(Jan) & year(2020)\n",
         "the range start becomes a holiday (h_ifand)"},
        {"version 19.5\ndateformat ymd\nrange 2020jan01 2020feb07\ncenterdate 2020jan01\n"
         "omit dayofweek We if month(Jan) | month(Feb)\n",
         "`|' joining if() conditions (h_ifor)"},
        {"version 19.5\ndateformat ymd\nrange 2020jan01 2020feb07\ncenterdate 2020jan01\n"
         "omit dayofweek (Sa Sa Su)\n",
         "a repeated day of week (h_dowdup)"},
        // z_win.stbcal verbatim (REFUTE-0 T6). dateformat dmy, so `16jun2020`
        // really parses -- an ymd spelling here would be refused as a bad DATE
        // and the leg would pass for the wrong reason.
        {"version 19.5\ndateformat dmy\nrange 02jan2020 31dec2020\ncenterdate 02jan2020\n"
         "omit dayofweek (Sa Su)\n"
         "from 16jun2020 to 30jun2020: omit date 16jun2020 and -1\n"
         "from 16jun2020 to 30jun2020: omit date 30jun2020 and +1\n",
         "a from/to window on a FULL-date omit (REFUTE-0 T6, z_win.stbcal)"},
        {"version 19.5\ndateformat ymd\ncenterdate 2020jan01\nrange 2020jan01 2020feb07\n",
         "centerdate before range"},
        {"", "an empty file (range undefined)"},
    };
    for (const auto& b : bad) {
      piet::bcal::ParseResult p = piet::bcal::parseStbcal(b.text, "probe");
      check(!p.ok && p.err.rc == 197,
            std::string("B2-3 bcal read: rc 197 for ") + b.what + " -- got " +
                (p.ok ? "ACCEPTED" : "rc " + std::to_string(p.err.rc)));
    }
    // ... and the shapes that must be ACCEPTED, including the manual's own
    // Christmas rules and the fifth-Monday case (arm A10).
    piet::bcal::ParseResult dim = piet::bcal::parseStbcal(
        "version 19.5\ndateformat dmy\nrange 02jan2020 30dec2021\ncenterdate 02jan2020\n"
        "omit dayofweek (Sa Su)\nomit dowinmonth -1 Mo of May\n",
        "dim");
    check(dim.ok && std::isnan(piet::bcal::bofd(dim.cal, td(2021, 5, 31))) &&
              !std::isnan(piet::bcal::bofd(dim.cal, td(2021, 5, 24))),
          "B2-3 bcal read: `-1 Mo of May' is 31may2021, the FIFTH Monday (arm A10 rejected)");
    piet::bcal::ParseResult star = piet::bcal::parseStbcal(
        "version 19.5\ndateformat ymd\nrange 2020jan02 2021dec31\ncenterdate 2020jan02\n"
        "omit dayofweek (Sa Su)\nomit date *jan01\nomit date *jul04\n",
        "star");
    check(star.ok && std::isnan(piet::bcal::bofd(star.cal, td(2021, 1, 1))) &&
              !std::isnan(piet::bcal::bofd(star.cal, td(2021, 1, 4))),
          "B2-3 bcal read: `*jan01' omits the date in every year of the range (h_star)");
    piet::bcal::ParseResult win = piet::bcal::parseStbcal(
        "version 19.5\ndateformat dmy\nrange 02jan2020 31dec2021\ncenterdate 02jan2020\n"
        "omit dayofweek (Sa Su)\nfrom . to 31dec2020: omit dayofweek Fr if month(Aug)\n",
        "win");
    check(win.ok && std::isnan(piet::bcal::bofd(win.cal, td(2020, 8, 7))) &&
              !std::isnan(piet::bcal::bofd(win.cal, td(2021, 8, 6))),
          "B2-3 bcal read: a from/to window really restricts the rule (arm A13 rejected)");
    piet::bcal::ParseResult offs = piet::bcal::parseStbcal(
        "version 19.5\ndateformat dmy\nrange 02jan2020 31dec2021\ncenterdate 02jan2020\n"
        "omit dayofweek (Sa Su)\nfrom 16jun2020 to 30jun2020: omit date 16jun* and -1\n"
        "from 16jun2020 to 30jun2020: omit date 30jun* and +1\n",
        "offs");
    check(offs.ok && std::isnan(piet::bcal::bofd(offs.cal, td(2020, 6, 15))) &&
              std::isnan(piet::bcal::bofd(offs.cal, td(2020, 7, 1))),
          "B2-3 bcal read: an `and +/-k' offset may land OUTSIDE the window (REFUTE-0 T7)");
    // The loader's default dateformat is dmy -- the manual says ymd and is
    // wrong about this loader (h_nodf_dmy loads, h_nodf_ymd does not).
    piet::bcal::ParseResult nodfDmy = piet::bcal::parseStbcal(
        "version 19.5\nrange 1/1/2020 2/7/2020\ncenterdate 1/1/2020\n", "nodf");
    piet::bcal::ParseResult nodfYmd = piet::bcal::parseStbcal(
        "version 19.5\nrange 2020jan01 2020feb07\ncenterdate 2020jan01\n", "nodf");
    check(nodfDmy.ok && nodfDmy.cal.hi == static_cast<std::int64_t>(td(2020, 7, 2)) &&
              !nodfYmd.ok,
          "B2-3 bcal read: with no `dateformat' line the default is DMY -- `2/7/2020' is 2 JULY "
          "(oracle max_date_td 22098), and an ymd-spelled date fails. The manual says ymd is "
          "assumed and is wrong about this loader.");
  }

  // --- 19..: the command surface, driven through dispatch ------------------
  auto errOf = [](const nlohmann::ordered_json& blocks) -> nlohmann::ordered_json {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "error") return b;
    return nlohmann::ordered_json::object();
  };
  {
    piet::SessionState s(workdir);
    s.eval.cwd = workdir;
    s.cwd = workdir;
    auto rcAfter = [&](const std::string& src) {
      (void)piet::doexec::executeDoSource("capture " + src + "\n", s, piet::dispatch);
      return s.eval.locals.get("_rc", "<unset>");
    };
    (void)piet::doexec::executeDoSource(
        "clear\nset obs 40\ngen bd = td(01jan2020) + _n - 1\ndrop if inlist(dow(bd), 1, 2)\n", s,
        piet::dispatch);
    auto out = piet::dispatch("bcal create selfwk, from(bd) replace", s);
    check(errOf(out).empty(), "B2-3 bcal: `bcal create NAME, from(v) replace' runs");
    check(fs::exists(fs::path(workdir) / "selfwk.stbcal"),
          "B2-3 bcal: the .stbcal lands in the session's cwd, not the scratch workdir");
    const double* minTd = s.eval.rValues.find("min_date_td");
    const double* maxTb = s.eval.rValues.find("max_date_tb");
    const double* omitted = s.eval.rValues.find("omitted");
    const double* oy = s.eval.rValues.find("omitted_year");
    check(minTd != nullptr && *minTd == 21915 && maxTb != nullptr && *maxTb == 29 &&
              omitted != nullptr && *omitted == 10,
          "B2-3 bcal: r(min_date_td)=21915, r(max_date_tb)=29, r(omitted)=10 (oracle a_r_*)");
    check(oy != nullptr && std::fabs(*oy - 91.3) < 1e-9,
          "B2-3 bcal: r(omitted_year) is rounded to one decimal, as Stata posts it");
    // The name resolves on every expression surface, through ONE loader.
    (void)piet::dispatch("gen b = bofd(\"selfwk\", bd)", s);
    (void)piet::dispatch("gen rt = dofb(b, \"selfwk\")", s);
    (void)piet::dispatch("count if rt != bd", s);
    const double* nBad = s.eval.rValues.find("N");
    check(nBad != nullptr && *nBad == 0,
          "B2-3 bcal: gen/dofb round trip over the whole column has 0 mismatches");
    (void)piet::dispatch("local dl = bofd(\"selfwk\", 21916)", s);
    check(s.eval.locals.get("dl", "") == "1",
          "B2-3 bcal: `local = bofd(...)' reaches the SAME loader as gen (evalArith's "
          "process namespace), got `" + s.eval.locals.get("dl", "<unset>") + "`");
    // Unknown / suffixed / prefixed names -- Stata answers missing with rc 0.
    (void)piet::dispatch("local u1 = bofd(\"nosuchcal\", 21916)", s);
    (void)piet::dispatch("local u2 = bofd(\"selfwk.stbcal\", 21916)", s);
    (void)piet::dispatch("local u3 = bofd(\"%tbselfwk\", 21916)", s);
    check(s.eval.locals.get("u1", "") == "." && s.eval.locals.get("u2", "") == "." &&
              s.eval.locals.get("u3", "") == "1",
          "B2-3 bofd: unknown name and a `.stbcal' SUFFIX are missing (rc 0); a `%tb' PREFIX "
          "names the same calendar");
    check(rcAfter("display bofd(\"selfwk\", \"21916\")") == "109",
          "B2-3 bofd: a STRING date is r(109) type mismatch (a_bofd_strdate_rc)");
    check(rcAfter("gen bad2 = bofd(21916, \"selfwk\")") == "109",
          "B2-3 bofd: swapped arguments are r(109) on the GEN surface too -- macros.cpp's "
          "stringFnArgSpecs row, the rule BUG-456's landing added (a_bofd_swapped_rc)");
    check(rcAfter("display bofd(\"selfwk\")") == "198" &&
              rcAfter("display bofd(\"selfwk\", 21916, 1)") == "198",
          "B2-3 bofd: arity 1 and arity 3 are r(198) (a_bofd_arity1_rc/_arity3_rc)");
    // NAMED DIVERGENCE (pre-existing, not bcal's): on the GEN surface an
    // exception raised INSIDE a function is reported as a generic evaluation
    // failure, rc 1 -- `gen x = substr("abcdef")` and `gen x = strlen("a","b")`
    // measure the same 1 on this build. The answer is still an error, never a
    // silent wrong number; only the code is coarse. BUGLOG-461.
    check(rcAfter("gen bad3 = bofd(\"selfwk\")") != "0",
          "B2-3 bofd: a bad-arity call on the GEN surface still ERRORS (rc 1 there, the shared "
          "pre-existing coarse code -- BUGLOG-461)");
    // A calendar name held in a STRING VARIABLE, per row.
    (void)piet::dispatch("gen str8 cn = \"selfwk\"", s);
    (void)piet::dispatch("gen bv = bofd(cn, bd)", s);
    (void)piet::dispatch("count if bv != b", s);
    const double* nv = s.eval.rValues.find("N");
    check(nv != nullptr && *nv == 0,
          "B2-3 bofd: the calendar name may be a string VARIABLE, resolved per row");
    // `format v %tbNAME` is accepted and `display %tbNAME i` renders the date.
    check(errOf(piet::dispatch("format b %tbselfwk", s)).empty(),
          "B2-3 bcal: `format v %tbNAME' is accepted (Stata accepts it even for an unknown "
          "calendar)");
    auto disp = piet::dispatch("display %tbselfwk 3", s);
    std::string dtext;
    for (const auto& b : disp)
      if (b.is_object() && b.value("type", std::string()) == "display")
        dtext = b.value("text", std::string());
    check(dtext.find("04jan2020") != std::string::npos,
          "B2-3 bcal: `display %tbNAME 3' prints the calendar date (a_disp_tb_3 = 04jan2020), "
          "got `" + dtext + "`");
    auto dispOut = piet::dispatch("display %tbselfwk 99", s);
    std::string otext;
    for (const auto& b : dispOut)
      if (b.is_object() && b.value("type", std::string()) == "display")
        otext = b.value("text", std::string());
    check(otext.find("99") != std::string::npos,
          "B2-3 bcal: an index OUTSIDE the calendar prints the raw number (a_disp_tb_99)");
    // File-level rcs.
    check(rcAfter("bcal create selfwk, from(bd)") == "602",
          "B2-3 bcal: an existing file without `replace' is r(602)");
    check(rcAfter("bcal create abcdefghijk, from(bd) replace") == "198",
          "B2-3 bcal: an 11-character calendar name is r(198)");
    check(rcAfter("bcal create selfwk.txt, from(bd) replace") == "198",
          "B2-3 bcal: an extension other than .stbcal is r(198)");
    check(rcAfter("bcal create 9bad, from(bd) replace") == "198",
          "B2-3 bcal: a name that is not a valid Stata name is r(198)");
    check(rcAfter("bcal create nodir/x, from(bd) replace") == "601",
          "B2-3 bcal: a path whose directory does not exist is r(601)");
    check(rcAfter("bcal describe nosuchcal") == "601",
          "B2-3 bcal: `bcal describe' on an unknown calendar is r(601)");
    // 06jan2020 is a MONDAY, and this calendar omits Monday and Tuesday, so
    // the centerdate lands on a business holiday -- g_sat's shape.
    check(rcAfter("bcal create selfsat, from(bd) centerdate(2020jan06) replace") == "197",
          "B2-3 bcal: a centerdate on an omitted weekday fails the self-describe with r(197) "
          "(g_sat_rc)");
    check(!fs::exists(fs::path(workdir) / "selfsat.stbcal"),
          "B2-3 bcal: the r(197) self-describe failure leaves NO file behind (g_sat_file_rc "
          "601)");
    // generate(): a long column formatted %tbNAME.
    check(errOf(piet::dispatch("bcal create selfgen, from(bd) generate(gv) replace", s)).empty(),
          "B2-3 bcal: `generate(newvar)' runs");
    check(s.varMeta.count("gv") != 0 && s.varMeta["gv"].format.has_value() &&
              *s.varMeta["gv"].format == "%tbselfgen",
          "B2-3 bcal: generate() formats the new variable %tbNAME");
    check(rcAfter("bcal create selfgen, from(bd) generate(gv) replace") == "110",
          "B2-3 bcal: generate() onto an EXISTING variable is r(110)");
    check(rcAfter("bcal create selfper, from(bd) personal replace") == "198",
          "B2-3 bcal: `personal' is refused by name -- PIET models no PERSONAL directory "
          "(named divergence D4), never silently written somewhere it cannot be found");
    // The cache refresh the paper depends on: a second create over a NARROWER
    // date set turns the later dates missing at the very next bofd.
    (void)piet::dispatch("local pre = bofd(\"selfwk\", td(09feb2020))", s);
    (void)piet::doexec::executeDoSource("drop if bd > td(20jan2020)\n", s, piet::dispatch);
    (void)piet::dispatch("bcal create selfwk, from(bd) replace", s);
    (void)piet::dispatch("local post = bofd(\"selfwk\", td(09feb2020))", s);
    check(s.eval.locals.get("pre", "") == "29" && s.eval.locals.get("post", "") == ".",
          "B2-3 bcal: a second `bcal create ..., replace' REFRESHES the cache -- the paper's "
          "38/80 hazard (pre=" + s.eval.locals.get("pre", "?") + ", post=" +
              s.eval.locals.get("post", "?") + ")");
    // bcal dir / load / check
    auto dirOut = piet::dispatch("bcal dir self*", s);
    check(errOf(dirOut).empty() &&
              s.eval.rStrings.get("calendars", "").find("selfwk") != std::string::npos,
          "B2-3 bcal: `bcal dir PATTERN' lists the cwd and fills r(calendars)");
    check(errOf(piet::dispatch("bcal load selfwk", s)).empty(),
          "B2-3 bcal: `bcal load' re-reads the file and reports success");
    (void)piet::dispatch("format b %tbnosuchcal", s);
    check(rcAfter("bcal check b") == "459",
          "B2-3 bcal: `bcal check' over a %tb format with no calendar is r(459)");
    check(rcAfter("bcal check b, rc0") == "0" &&
              s.eval.rStrings.get("undefined", "") == "nosuchcal",
          "B2-3 bcal: `bcal check, rc0' is rc 0 with r(undefined) naming the calendar");
  }
  {  // no data in memory: `bcal create` is Stata's rc 3, not a refusal
    piet::SessionState s(workdir);
    s.eval.cwd = workdir;
    s.cwd = workdir;
    (void)piet::doexec::executeDoSource("capture bcal create nodatacal, from(bd)\n", s,
                                        piet::dispatch);
    check(s.eval.locals.get("_rc", "") == "3",
          "B2-3 bcal: `bcal create' with nothing in memory is r(3) `no dataset in use', got _rc=" +
              s.eval.locals.get("_rc", "<unset>"));
  }

  // --- the calendar-NAME rule on the READ path -----------------------------
  // [D] bcal printed p.29: "calname has to be a valid Stata name but limited to
  // 10 characters". The landing enforced it only in the create branch, so PIET
  // ANSWERED where Stata refuses -- and on the data plane it answered a NUMBER
  // where Stata answers missing. Every rc below is measured, StataBE 19.5 on
  // this machine, with the SAME valid five-line .stbcal present in the cwd
  // (/private/tmp/bcalfix/stata/s1.log, s2.log, s3.log; the review's
  // /private/tmp/bcalrev/stata/probe2.log agrees).
  {
    const std::string namedir = (fs::path(workdir) / "names").u8string();
    fs::create_directories(namedir, ec);
    static const char* kValidCal =
        "version 19.5\n"
        "dateformat ymd\n"
        "range 2020jan01 2020feb07\n"
        "centerdate 2020jan01\n"
        "omit dayofweek (Sa Su)\n";
    for (const char* nm : {"elevenchars", "9bad", "tenchars10"}) {
      std::ofstream f((fs::path(namedir) / (std::string(nm) + ".stbcal")).u8string(),
                      std::ios::binary);
      f << kValidCal;
    }
    piet::SessionState s(namedir);
    s.eval.cwd = namedir;
    s.cwd = namedir;
    auto rcAfter = [&](const std::string& src) {
      (void)piet::doexec::executeDoSource("capture " + src + "\n", s, piet::dispatch);
      return s.eval.locals.get("_rc", "<unset>");
    };
    auto valOf = [&](const std::string& expr) {
      (void)piet::doexec::executeDoSource("local __v = " + expr + "\n", s, piet::dispatch);
      return s.eval.locals.get("__v", "<unset>");
    };
    check(rcAfter("bcal load elevenchars") == "198" &&
              rcAfter("bcal describe elevenchars") == "198",
          "B2-3 bcal name rule: an 11-character name is r(198) on BOTH read verbs, with a "
          "perfectly readable file right there (e11_load_rc/e11_desc_rc = 198)");
    check(rcAfter("bcal load 9bad") == "198" && rcAfter("bcal describe 9bad") == "198",
          "B2-3 bcal name rule: a name that is not a valid Stata name is r(198) on both read "
          "verbs (b9_load_rc = 198)");
    check(rcAfter("bcal describe %tbelevenchars") == "198",
          "B2-3 bcal name rule: the rule is applied AFTER the %tb strip -- `bcal describe "
          "%tbelevenchars' is r(198), not a lookup of a name called `%tbelevenchars'");
    check(rcAfter("bcal load elevenchr11") == "198",
          "B2-3 bcal name rule: the name rule beats the missing-file rule -- an 11-character "
          "name with NO file is r(198), not r(601) (measured, s3.log M01)");
    check(rcAfter("bcal load nosuchcal") == "601",
          "B2-3 bcal name rule: a LEGAL name with no file keeps its own r(601) -- the two "
          "refusals are distinct in Stata and stay distinct here");
    check(valOf("bofd(\"elevenchars\", 21916)") == "." &&
              valOf("bofd(\"9bad\", 21916)") == "." &&
              valOf("dofb(1, \"elevenchars\")") == ".",
          "B2-3 bcal name rule: the DATA plane answers MISSING with rc 0 for an invalid name "
          "-- it answered 1 before this fix (e11_bofd/b9_bofd = `.')");
    check(rcAfter("bcal load tenchars10") == "0" && valOf("bofd(\"tenchars10\", 21916)") == "1",
          "B2-3 bcal name rule: TEN characters is inside the rule -- the boundary is <= 10, "
          "not < 10 (measured, s1.log R09/R10)");
    (void)piet::doexec::executeDoSource(
        "clear\nset obs 5\ngen bd = td(01jan2020) + _n - 1\n"
        "gen b11 = bofd(\"elevenchars\", bd)\ncount if missing(b11)\n",
        s, piet::dispatch);
    const double* nMiss = s.eval.rValues.find("N");
    check(nMiss != nullptr && *nMiss == 5,
          "B2-3 bcal name rule: over 5 rows ALL five are missing (Stata 5; this branch answered "
          "2 before the fix) -- the silent-wrong-answer half of the finding");
    auto dirOut = piet::dispatch("bcal dir *", s);
    const std::string cals = s.eval.rStrings.get("calendars", "");
    check(errOf(dirOut).empty() && cals.find("elevenchars") != std::string::npos,
          "B2-3 bcal name rule: `bcal dir' does NOT apply it -- an invalid-named .stbcal is "
          "still listed (e11_dir_cals), got `" + cals + "`");
    // --- the option net, per sub-command ----------------------------------
    // kVocabBcal is the UNION of five `syntax` lines; rejecting against it
    // before the sub-command is resolved let every sub-command swallow every
    // other sub-command's options as a silent no-op. All seven of these
    // answered rc 0 before the fix.
    check(rcAfter("bcal describe tenchars10, maxgap(5)") == "101" &&
              rcAfter("bcal describe tenchars10, from(bd)") == "101" &&
              rcAfter("bcal describe tenchars10, purpose(\"x\")") == "101",
          "B2-3 bcal options: `bcal describe' is a BARE `syntax' -- ANY option is r(101) "
          "`options not allowed'");
    check(rcAfter("bcal load tenchars10, replace") == "101",
          "B2-3 bcal options: `bcal load' is a bare `syntax' too -- r(101)");
    check(rcAfter("bcal describe elevenchars, maxgap(5)") == "101",
          "B2-3 bcal options: the OPTION error beats the calendar-name error, as in Stata "
          "(measured 101, not 198)");
    check(rcAfter("bcal dir, rc0") == "198",
          "B2-3 bcal options: `bcal dir' takes no rc0 -- r(198)");
    check(rcAfter("bcal check, maxgap(4)") == "198" && rcAfter("bcal check, rc0") == "0",
          "B2-3 bcal options: `bcal check' takes rc0 and NOTHING else -- maxgap() is r(198), "
          "rc0 is rc 0");
    check(rcAfter("bcal create c1, from(bd) replace rc0") == "198",
          "B2-3 bcal options: `bcal create' has no rc0 -- r(198)");
    check(rcAfter("bcal dir, testmode") != "0",
          "B2-3 bcal options: `bcal dir, testmode' is bcal_dir's undocumented hook (Stata rc 0, "
          "re-measured 2026-09-09 in BOTH a populated and an empty cwd -- cap2.log "
          "D_dir_testmode_rc / cap3.log E_testmode_empty_rc; the review's r(3300) does not "
          "reproduce here); PIET has no such mode and refuses it BY NAME, not as a no-op");
    // --- non-blocking review items, pinned so the agreement is not accidental
    (void)piet::doexec::executeDoSource(
        "clear\nset obs 5\ngen obd = td(02jan2020)\n", s, piet::dispatch);
    check(rcAfter("bcal create onecal, from(obd) replace") == "197" &&
              !fs::exists(fs::path(namedir) / "onecal.stbcal"),
          "B2-3 bcal: a from() column with ONE distinct date is r(197) from the self-describe, "
          "as in Stata -- there is no writer-side range guard (measured 197, "
          "/private/tmp/bcalrev/stata/probe4.log one_rc)");
    piet::bcal::ParseResult vc = piet::bcal::parseStbcal(
        "version 19.5 // trailing comment\nrange 2020jan01 2020feb07\n"
        "centerdate 2020jan01\n",
        "vercmt");
    check(!vc.ok && vc.err.rc == 197,
          "B2-3 bcal read: `version 19.5 // trailing comment' -- the [D] manual's OWN listing "
          "style -- is r(197) in StataBE (vercmt_rc, REFUTE-1 SC) and r(197) here; the "
          "agreement is pinned, not accidental");
  }

  // --- the calendar-NAME rule on the FORMAT plane --------------------------
  // Review 2026-09-09, BLOCKING 2. The landing bound the 10-character rule on
  // the create plane and (in the same commit's review pass) on the read plane,
  // but `format x %tbNAME` -- the third surface the same commit added -- took
  // ANY identifier. That is a LENIENCY divergence, which this repo's own
  // adjudication law defaults to refusing. Every rc below is StataBE 19.5 on
  // this machine, /private/tmp/fix-bcal/st/cap.log keys A_*:
  //   A_ten_rc 0 · A_ten_detail_rc 0 · A_one_rc 0 · A_eleven2_rc 0
  //   A_eleven_rc 120 · A_eleven_detail_rc 120 · A_dash_rc 120
  //   A_digitstart_rc 120 · A_zz_rc 120
  // The 120 also fixes a SECOND, pre-existing gap the review named (its
  // non-blocking 8): every invalid format used to leave here as the generic
  // engine_error, i.e. rc 1, because "invalid_format" is not one of the frozen
  // error codes. `%zz` measures 120 in Stata too, so the whole refusal now
  // carries the rc rather than the %tb arm carrying a special case.
  {
    const std::string fmtdir = (fs::path(workdir) / "fmt").u8string();
    fs::create_directories(fmtdir, ec);
    piet::SessionState s(fmtdir);
    s.eval.cwd = fmtdir;
    s.cwd = fmtdir;
    auto rcAfter = [&](const std::string& src) {
      (void)piet::doexec::executeDoSource("capture " + src + "\n", s, piet::dispatch);
      return s.eval.locals.get("_rc", "<unset>");
    };
    (void)piet::doexec::executeDoSource("clear\nset obs 3\ngen x = _n\n", s, piet::dispatch);
    check(rcAfter("format x %tbelevenchars") == "120",
          "B2-3 format name rule: an 11-character calendar name in a %tb format is r(120) -- "
          "this branch ACCEPTED it at rc 0 before the fix (oracle A_eleven_rc = 120)");
    check(rcAfter("format x %tbelevenchars:day") == "120",
          "B2-3 format name rule: the detail-coded form is judged on the NAME, not on the "
          "whole spec (A_eleven_detail_rc = 120)");
    check(rcAfter("format x %tbabcdefghij") == "0" &&
              rcAfter("format x %tbabcdefghij:day") == "0",
          "B2-3 format name rule: TEN characters is inside the rule, with and without a detail "
          "code -- the boundary is <= 10 (A_ten_rc / A_ten_detail_rc = 0)");
    check(rcAfter("format x %tba") == "0",
          "B2-3 format name rule: a one-character name is fine (A_one_rc = 0)");
    check(rcAfter("format x %tbab-cd") == "120" && rcAfter("format x %tb9abc") == "120",
          "B2-3 format name rule: a name that is not a valid Stata name is r(120) -- and it is "
          "r(120), not the generic rc 1 this build used to answer (A_dash_rc/A_digitstart_rc)");
    check(rcAfter("format x %zz") == "120",
          "B2-3 format: EVERY unrecognised %fmt is r(120), not rc 1 -- the review's "
          "non-blocking 8 class, closed by the same refusal (A_zz_rc = 120)");
    check(rcAfter("format x %td") == "0" && rcAfter("format x %9.2f") == "0" &&
              rcAfter("format x %tbtenchars10") == "0",
          "B2-3 format: the accepting arms are untouched by the r(120) change");
  }

  // --- the calendar cache is keyed by NAME, and `cd` does not drop it ------
  // Review 2026-09-09, BLOCKING 1. The cache key used to be (search directory,
  // name), and runCd re-points the search directory on every `cd` -- so a `cd`
  // SILENTLY DROPPED a loaded calendar (missing, rc 0) and a `cd` back
  // resurrected a stale one (wrong value, rc 0). Both are Doctrine-(1). Stata's
  // cache is NAME-keyed; measured this review, StataBE 19.5,
  // /private/tmp/fix-bcal/st/cap.log:
  //   B_before 1 · B_in_sub 1 · B_sub_recreate -1 · B_back -1
  //   C_after_erase 1 · C_in_sub 1 · C_back 1   (the file is gone throughout)
  // and the OTHER direction still has to hold -- a not-yet-loaded calendar is
  // found in the new cwd (capture2.log l_cd_bofd_in_sub = 1), which is why
  // `cd` still re-points the search DIRECTORY. Resolution is by directory;
  // memory is by name.
  {
    const std::string cddir = (fs::path(workdir) / "cd").u8string();
    fs::create_directories(fs::path(cddir) / "sub", ec);
    piet::SessionState s(cddir);
    s.eval.cwd = cddir;
    s.cwd = cddir;
    auto run = [&](const std::string& src) {
      (void)piet::doexec::executeDoSource(src, s, piet::dispatch);
    };
    auto valOf = [&](const std::string& expr) {
      (void)piet::doexec::executeDoSource("local __v = " + expr + "\n", s, piet::dispatch);
      return s.eval.locals.get("__v", "<unset>");
    };
    run("clear\nset obs 30\ngen bd = td(01jan2020) + _n - 1\nformat bd %td\n");
    run("quietly bcal create cdcal, from(bd) replace\n");
    const std::string before = valOf("bofd(\"cdcal\", td(02jan2020))");
    run("cd sub\n");
    const std::string inSub = valOf("bofd(\"cdcal\", td(02jan2020))");
    check(before == "1" && inSub == "1",
          "B2-3 bcal cache: a `cd' does NOT drop a loaded calendar -- Stata answers 1 in the "
          "subdirectory (B_in_sub); this branch answered `.', a MISSING at rc 0, got `" +
              inSub + "'");
    run("quietly bcal create cdcal, from(bd) centerdate(2020jan03) replace\n");
    const std::string subRecreate = valOf("bofd(\"cdcal\", td(02jan2020))");
    run("cd ..\n");
    const std::string back = valOf("bofd(\"cdcal\", td(02jan2020))");
    check(subRecreate == "-1" && back == "-1",
          "B2-3 bcal cache: a `bcal create' refreshes the cache BY NAME, so coming back out of "
          "the subdirectory still answers the SUB calendar's -1 (B_sub_recreate/B_back) -- this "
          "branch answered 1 from the stale cwd-keyed entry, a WRONG value at rc 0, got `" +
              back + "'");
    // The erase arm: the FILE is irrelevant once the calendar is in the cache,
    // across a cd in both directions (C_after_erase/C_in_sub/C_back = 1).
    run("quietly bcal create ecal, from(bd) replace\n");
    fs::remove(fs::path(cddir) / "ecal.stbcal", ec);
    const std::string afterErase = valOf("bofd(\"ecal\", td(02jan2020))");
    run("cd sub\n");
    const std::string eraseInSub = valOf("bofd(\"ecal\", td(02jan2020))");
    run("cd ..\n");
    const std::string eraseBack = valOf("bofd(\"ecal\", td(02jan2020))");
    check(afterErase == "1" && eraseInSub == "1" && eraseBack == "1",
          "B2-3 bcal cache: erasing the .stbcal changes nothing, and a `cd' out and back does "
          "not change that either (C_after_erase/C_in_sub/C_back = 1)");
    // The other direction, unchanged by the key: a calendar NEVER loaded is
    // resolved against the NEW cwd after a cd (oracle l_cd_bofd_in_sub = 1).
    {
      std::ofstream f((fs::path(cddir) / "sub" / "subonly.stbcal").u8string(), std::ios::binary);
      f << "version 19.5\ndateformat ymd\nrange 2020jan01 2020feb07\n"
           "centerdate 2020jan01\nomit dayofweek (Sa Su)\n";
    }
    const std::string beforeCd = valOf("bofd(\"subonly\", td(02jan2020))");
    run("cd sub\n");
    const std::string afterCd = valOf("bofd(\"subonly\", td(02jan2020))");
    run("cd ..\n");
    check(beforeCd == "." && afterCd == "1",
          "B2-3 bcal cache: `cd' still re-points the SEARCH DIRECTORY -- an unloaded calendar "
          "in the new cwd is found there (l_cd_bofd_in_sub = 1) and was missing before the cd");
    // Session isolation is the RESET's job now, not the key's: the cache holds
    // `cdcal' and `ecal' with no file behind ecal, and a NEW SessionState must
    // not inherit either. This is what SessionState::resetProcessCaches() buys;
    // before this review bcal::clearCache() had zero callers and the isolation
    // was carried, by accident, by the search directory sitting in the key.
    {
      const std::string isodir = (fs::path(workdir) / "iso").u8string();
      fs::create_directories(isodir, ec);
      piet::SessionState s2(isodir);
      s2.eval.cwd = isodir;
      s2.cwd = isodir;
      (void)piet::doexec::executeDoSource("local __v = bofd(\"ecal\", td(02jan2020))\n", s2,
                                          piet::dispatch);
      check(s2.eval.locals.get("__v", "<unset>") == ".",
            "B2-3 bcal cache: a NEW session does not inherit the previous session's calendars "
            "-- `ecal' has no file, so only the process cache could have answered");
    }
  }

  // --- r(fn) is a presentation string, and %tb pads its fallbacks ----------
  // Review 2026-09-09, non-blocking 3 and 4. Both are measured, StataBE 19.5
  // (/private/tmp/fix-bcal/st/cap2.log D_create_r_fn / D_desc_r_fn / D_dir_fn,
  // cap4.log F*).
  {
    const std::string pdir = (fs::path(workdir) / "pres").u8string();
    fs::create_directories(pdir, ec);
    piet::SessionState s(pdir);
    s.eval.cwd = pdir;
    s.cwd = pdir;
    auto run = [&](const std::string& src) {
      (void)piet::doexec::executeDoSource(src, s, piet::dispatch);
    };
    run("clear\nset obs 30\ngen bd = td(01jan2020) + _n - 1\nformat bd %td\n");
    run("quietly bcal create a_wk, from(bd) replace\n");
    const std::string createFn = s.eval.rStrings.get("fn", "<unset>");
    run("quietly bcal describe a_wk\n");
    const std::string descFn = s.eval.rStrings.get("fn", "<unset>");
    run("quietly bcal dir a_wk\n");
    const std::string dirFn = s.eval.rStrings.get("fn_a_wk", "<unset>");
    check(createFn == "a_wk.stbcal" && descFn == "./a_wk.stbcal" && dirFn == "./a_wk.stbcal",
          "B2-3 bcal r(fn): create posts the BARE name, describe and dir post `./NAME.stbcal' "
          "-- describe/dir posted the ABSOLUTE machine path before this fix, got `" + createFn +
              "' / `" + descFn + "' / `" + dirFn + "'");
    // `: display %fmt v' (the extended macro function) is not a surface this
    // build has, so the pin is taken off the `display' block itself.
    auto dispOf = [&](const std::string& spec, const std::string& val) {
      auto blocks = piet::dispatch("display \"[\" " + spec + " " + val + " \"]\"", s);
      std::string text;
      if (blocks.is_array()) {
        for (const auto& b : blocks)
          if (b.is_object() && b.value("type", std::string()) == "display")
            text = b.value("text", std::string());
      } else if (blocks.is_object() && blocks.value("type", std::string()) == "display") {
        text = blocks.value("text", std::string());
      }
      return text;
    };
    check(dispOf("%tba_wk", "3") == "[04jan2020]" && dispOf("%tba_wk", "-1") == "[       -1]" &&
              dispOf("%tba_wk", "99") == "[       99]" && dispOf("%tba_wk", ".") == "[        .]",
          "B2-3 %tb display: an out-of-range index and a missing fall back to the raw number "
          "RIGHT-ALIGNED in width 9, not flush left (cap4.log Fneg/F99/Fmiss)");
    check(dispOf("%tbnosuchcl", "3") == "[        3]" &&
              dispOf("%tbnosuchcl", "-1") == "[       -1]",
          "B2-3 %tb display: the unloadable-calendar fallback is padded too (cap4.log "
          "Fnocal/Fnocalneg) -- the width is the same 9 whether the calendar is missing or the "
          "index is");
  }

  fs::remove_all(workdir, ec);
}
