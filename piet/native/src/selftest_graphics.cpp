// SPDX-License-Identifier: GPL-3.0-or-later
// Existing behavioral tests: graphics.
#include "selftest_support.h"
#include "selftest_cases.h"


// ── AER-SCOPE1 bundle J (GRAPH-X, 2026-09-04) ────────────────────────────────
//
// SIX registry rows, not the plan's fourteen: grc1leg2, grstyle, geodist,
// geonear, cdfplot, event_plot. `grc1leg` (un-suffixed) and the eight refusals
// (cibar binsreg spmap maptile shp2dta geoinpoly heatplot distplot) are
// UNREGISTERED -- the attribution rule gives a row only to a package that
// states its own author -- so they answer a GENERIC r(199). Leg (xv) asserts
// exactly that generic shape and nothing more: a leg pinning a specific
// `reason` string for them would be asserting a promise this tree does not
// make, and it would keep passing after somebody registered them under a
// different cause.
//
// WHAT MAKES THIS BUNDLE DANGEROUS is that every one of its wrong
// implementations returns rc 0 and draws a picture:
//   - a grc1leg2 that combined the panels and set nothing produces a VALID
//     combine block with every legend still on -- the exact picture the
//     command exists to prevent (leg i);
//   - a geodist that shipped haversine as the default is wrong by 0.07% and
//     no chart shows it (legs ii/iv);
//   - an ECDF that ends at (n-1)/n or n/(n+1) is the classic off-by-one and
//     looks right on a plot (leg x);
//   - an event_plot that forwarded without the stub filter draws a plausible
//     plot of the WRONG coefficients (leg xiii);
//   - a grstyle that silently no-ops is the accept-note behaviour with the
//     evidence removed (leg xii).
// So no leg below stops at a return code. Each asserts a VALUE or a BLOCK
// STRUCTURE, and each names, in its own comment, the wrong implementation it
// separates from the right one.
//
// TWO TRAPS THIS FUNCTION PAYS FOR UP FRONT.
//   1. `session.cwd` is seeded as well as `session.eval.cwd`: resolveUserPath
//      (handlers.cpp:2040) reads SessionState::cwd, and `geonear` resolves a
//      `using` file through it. Bundle E seeded only eval.cwd and sixteen
//      checks failed for that one reason.
//   2. Every numeric report goes through `fullPrec` (%.17g). std::to_string
//      prints six decimals, which renders a genuine 8th-digit difference as
//      two identical strings -- and a distance check whose failure message
//      reads `321.218660 vs 321.218660` is not a failure report.
//
// Nothing here asserts a PATH: the workdir is this process's temp directory.
void testAerGraphX() {
  std::printf("[selftest] --- AER-SCOPE1 bundle J: grc1leg2 / grstyle / geodist / geonear / "
              "cdfplot / event_plot + the unregistered generic-199 names ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_aergraphx").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  auto exec = [](const std::string& src, piet::SessionState& session) {
    return piet::doexec::executeDoSource(src, session, piet::dispatch);
  };
  auto seed = [&](piet::SessionState& s) {
    s.eval.cwd = workdir;
    s.cwd = workdir;  // resolveUserPath reads THIS field (handlers.cpp:2040).
  };
  // Returns a NULL json rather than throwing when there is no error block --
  // the BUG-129 trap, and check()'s message argument is built EAGERLY (on the
  // passing path too), so every message below reads through these helpers.
  auto firstError = [](const nlohmann::ordered_json& blocks) -> nlohmann::ordered_json {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "error") return b;
    return nlohmann::ordered_json();
  };
  auto errText = [](const nlohmann::ordered_json& e) {
    return e.is_object() ? e.value("message", std::string()) : std::string("<none>");
  };
  auto errField = [](const nlohmann::ordered_json& e, const std::string& f) {
    return e.is_object() ? e.value(f, std::string()) : std::string("<none>");
  };
  auto rcOf = [](const nlohmann::ordered_json& e) {
    return e.is_object() ? piet::eval::coarseRc(e) : std::string("<none>");
  };
  // See the header: %.17g, because `321.218660` twice is not a failure report.
  auto fullPrec = [](double v) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return std::string(buf);
  };
  auto has = [](const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
  };
  auto spit = [](const std::string& p, const std::string& bytes) {
    std::ofstream f(cecon::to_native_path(p), std::ios::binary | std::ios::trunc);
    f << bytes;
  };
  auto displays = [](const auto& blocks) {
    std::vector<std::string> v;
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "display")
        v.push_back(b.value("text", std::string()));
    return v;
  };
  // The first `type == "graph"` block, or NULL. Every accessor below is
  // type-guarded for the same reason firstError is: a missing key must fail
  // ONE check with a readable message, not throw out of the whole run.
  auto graphBlock = [](const auto& blocks) -> nlohmann::ordered_json {
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "graph") return b;
    return nlohmann::ordered_json();
  };
  auto gstr = [](const nlohmann::ordered_json& j, const std::string& k) {
    return (j.is_object() && j.contains(k) && j[k].is_string()) ? j[k].get<std::string>()
                                                                : std::string("<absent>");
  };
  auto garr = [](const nlohmann::ordered_json& j,
                 const std::string& k) -> nlohmann::ordered_json {
    return (j.is_object() && j.contains(k) && j[k].is_array())
               ? j[k]
               : nlohmann::ordered_json::array();
  };
  auto gobj = [](const nlohmann::ordered_json& j,
                 const std::string& k) -> nlohmann::ordered_json {
    return (j.is_object() && j.contains(k) && j[k].is_object())
               ? j[k]
               : nlohmann::ordered_json::object();
  };
  auto gbool = [](const nlohmann::ordered_json& j, const std::string& k, bool dflt) {
    return (j.is_object() && j.contains(k) && j[k].is_boolean()) ? j[k].get<bool>() : dflt;
  };
  // points[i] is a two-element [x, y] array; `which` is 0 for x, 1 for y.
  auto ptAt = [](const nlohmann::ordered_json& points, size_t i, size_t which) -> double {
    if (!points.is_array() || i >= points.size()) return std::nan("");
    const auto& p = points[i];
    if (!p.is_array() || which >= p.size() || !p[which].is_number()) return std::nan("");
    return p[which].get<double>();
  };
  // r() is REPLACED wholesale by a summarize (macros.cpp's captureRFromBlock),
  // so every read below happens immediately after the command that published
  // it -- never two commands later.
  auto rOf = [](piet::SessionState& s, const char* key) -> double {
    const double* p = s.eval.rValues.find(key);
    return p ? *p : std::nan("");
  };
  auto runR = [&](piet::SessionState& s, const std::string& cmd, const char* key) -> double {
    (void)piet::dispatch(cmd, s);
    return rOf(s, key);
  };
  auto countIf = [&](piet::SessionState& s, const std::string& expr) {
    return runR(s, "count if " + expr, "N");
  };
  auto sumOf = [&](piet::SessionState& s, const std::string& var) {
    return runR(s, "summarize " + var, "sum");
  };
  auto meanOf = [&](piet::SessionState& s, const std::string& var) {
    return runR(s, "summarize " + var, "mean");
  };
  auto varNames = [](piet::SessionState& s) {
    std::vector<std::string> out;
    const nlohmann::ordered_json blocks = piet::dispatch("describe", s);
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) == "describe" && b.contains("variables"))
        for (const auto& v : b["variables"]) out.push_back(v.value("name", std::string()));
    return out;
  };
  auto hasName = [](const std::vector<std::string>& v, const std::string& n) {
    return std::find(v.begin(), v.end(), n) != v.end();
  };
  auto joinNames = [](const std::vector<std::string>& v) {
    std::string s;
    for (const auto& n : v) { if (!s.empty()) s += " "; s += n; }
    return s.empty() ? std::string("<none>") : s;
  };

  // The three graphs legs (i) and (xv) build. Titles, not names, are what the
  // panel objects carry into the combine block, so the title is how a panel is
  // identified below.
  const std::string kThreeGraphs =
      "clear\n"
      "set obs 6\n"
      "gen gx = _n\n"
      "gen gy = 2*_n\n"
      "twoway scatter gy gx, name(g1) title(\"T1\")\n"
      "twoway line gy gx, name(g2) title(\"T2\")\n"
      "twoway connected gy gx, name(g3) title(\"T3\")\n";

  // Counts the panels whose legend is still ON, and reports which one it is.
  auto keptLegend = [&](const nlohmann::ordered_json& blk, std::string& whichTitle) {
    int kept = 0;
    whichTitle = "<none>";
    for (const auto& p : garr(blk, "panels")) {
      if (!gbool(gobj(p, "legend"), "off", false)) {
        ++kept;
        if (kept == 1) whichTitle = gstr(p, "title");
      }
    }
    return kept;
  };

  // ── (i) grc1leg2 keeps EXACTLY ONE legend. ────────────────────────────────
  // This is the whole command. A shim that resolved the names, called
  // runGraphCombine and set nothing produces a structurally valid
  // `graph_combine` block with three panels at rc 0 -- and three legends, which
  // is the picture the user ran grc1leg2 to avoid. The count is therefore the
  // assertion, not the block's existence: 3 is "did nothing", 0 is "switched
  // them all off and left the plot legend-less", and 1 is the answer. WHICH one
  // survives is the second half: the default is the FIRST panel, so a handler
  // that kept the last one agrees with the count check and disagrees here.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec(kThreeGraphs, s);
    auto r = exec("grc1leg2 g1 g2 g3\n", s);
    const nlohmann::ordered_json blk = graphBlock(r.blocks);
    check(gstr(blk, "kind") == "graph_combine",
          "AER-GRAPHX (i): `grc1leg2 g1 g2 g3` emits a graph_combine block (the same block "
          "`graph combine` emits -- this row is a shim over runGraphCombine, not a new "
          "renderer). Got kind `" + gstr(blk, "kind") + "`");
    check(garr(blk, "panels").size() == 3,
          "AER-GRAPHX (i): ...carrying all THREE panels. Got " +
              std::to_string(garr(blk, "panels").size()));
    std::string which;
    const int kept = keptLegend(blk, which);
    check(kept == 1,
          "AER-GRAPHX (i): ...and EXACTLY ONE panel keeps its legend. 3 here means the shim "
          "combined the graphs and set nothing -- a valid block, rc 0, and the wrong picture; "
          "0 means it switched every legend off and the combined plot has none at all. Got " +
              std::to_string(kept));
    check(which == "T1",
          "AER-GRAPHX (i): ...and with no legendfrom() it is the FIRST panel, per the plan's "
          "default. A handler that kept the last panel's legend passes the count check and "
          "fails here. Got `" + which + "`");
  }

  // ── (ii) legendfrom() moves the surviving legend. ─────────────────────────
  // Without this leg, legendfrom() could be parsed, accepted and dropped and
  // every check above would still pass -- the option would be decoration.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec(kThreeGraphs, s);
    auto r = exec("grc1leg2 g1 g2 g3, legendfrom(g2)\n", s);
    const nlohmann::ordered_json blk = graphBlock(r.blocks);
    std::string which;
    const int kept = keptLegend(blk, which);
    check(kept == 1,
          "AER-GRAPHX (ii): `legendfrom(g2)` still leaves exactly one legend on. Got " +
              std::to_string(kept));
    check(which == "T2",
          "AER-GRAPHX (ii): ...and it is g2's, not the default first panel's -- otherwise "
          "legendfrom() is an option the parser accepts and the handler ignores. Got `" +
              which + "`");

    // The panels are COPIES. grc1leg2 sets legend.off on the panel objects
    // inside its own combine block; if it wrote through to
    // session.namedGraphs instead, the user's `g1`/`g3` would come back
    // legend-less from every LATER command -- a mutation nobody looks for
    // because the command that caused it succeeded. A plain `graph combine`
    // afterwards must still find all three legends on.
    auto after = exec("graph combine g1 g2 g3\n", s);
    const nlohmann::ordered_json blk2 = graphBlock(after.blocks);
    std::string which2;
    const int kept2 = keptLegend(blk2, which2);
    check(kept2 == 3,
          "AER-GRAPHX (ii): ...and the SOURCE graphs are untouched -- a later `graph combine "
          "g1 g2 g3` still finds three legends. Fewer means grc1leg2 mutated "
          "session.namedGraphs in place and quietly stripped the user's own graphs. Got " +
              std::to_string(kept2));
  }

  // ── (iii) geodist: a KNOWN distance, to a tolerance that discriminates. ───
  // Seoul (37.5, 127.0) -> Busan (35.1, 129.0):
  //     WGS84 Vincenty inverse   321.218660 km   <- the contracted default
  //     haversine, R=6371        321.447478 km   <- what `sphere` must give
  // The two differ by 0.229262 km (0.0714%), so the band below is +/-0.10 km:
  // wide enough that no correct Vincenty can miss it (converged to 1e-12 rad
  // the answer is stable to millimetres) and narrow enough that shipping
  // haversine as the DEFAULT lands 2.3 tolerances away and fails. That is the
  // whole point of choosing the number rather than asserting "a column
  // appeared": a haversine default is right to three significant figures and
  // wrong, and no plot of it looks wrong.
  //
  // NOTE FOR THE PLAN'S READER: the ONESHOT plan's Done-When says this pair is
  // "~325.0 km". It is not -- 325 km is the Seoul-Busan CITY-CENTRE distance,
  // not the distance between the rounded coordinates the plan itself writes.
  // The number pinned here was computed independently on both sides (this lane
  // and the handler lane) before either was written.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec("clear\nset obs 1\n", s);
    // BUG-426 (2026-09-06): four LITERAL coordinates are the IMMEDIATE form —
    // geodist.ado prints the distance and returns r(distance), and generate()
    // there is r(198). This leg used to drive the literal+gen() spelling, which
    // is precisely the inversion the oracle caught, so it now reads r(distance)
    // and the variable form is exercised by leg (iv) below.
    auto r = exec("geodist 37.5 127.0 35.1 129.0\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-GRAPHX (iii): `geodist 37.5 127.0 35.1 129.0` runs clean as the IMMEDIATE form "
          "(four literals, no generate()). Got `" + errText(firstError(r.blocks)) + "`");
    const double km = runR(s, "", "distance");
    check(std::isfinite(km) && std::fabs(km - 321.218660) < 0.10,
          "AER-GRAPHX (iii): ...and the value is the WGS84 Vincenty distance 321.218660 km. "
          "The tolerance is 0.10 km ON PURPOSE: haversine (R=6371.0088) answers 321.447922 for "
          "this pair, 0.229 km away, so a haversine default fails this check instead of passing "
          "it three significant figures deep. Got " + fullPrec(km));

    // `sphere` is a DIFFERENT function, not a synonym. Asserting only the
    // Vincenty value would leave a build in which both spellings return the
    // same number entirely green.
    auto r2 = exec("geodist 37.5 127.0 35.1 129.0, sphere\n", s);
    check(firstError(r2.blocks).is_null(),
          "AER-GRAPHX (iii): `sphere` is accepted. Got `" + errText(firstError(r2.blocks)) + "`");
    const double sph = runR(s, "", "distance");
    // TIGHT, and deliberately so: 1e-4 km. The 0.10 km tolerance on the line
    // above discriminates haversine-from-Vincenty (0.229 km apart); it cannot
    // see the RADIUS, and the radius is its own contract. geodist uses a
    // radius of exactly 6371 km (matches geodist's behaviour, identified from
    // its ado), not the IUGG mean 6371.0088 the plan
    // drafted -- a 0.000444 km difference here, invisible in any rendered
    // output, which the wider tolerance silently absorbed when the constant was
    // corrected. A pin that cannot fail on the number it names is not pinning
    // it.
    check(std::isfinite(sph) && std::fabs(sph - 321.447478) < 1e-4,
          "AER-GRAPHX (iii): ...and `sphere` is haversine on the PACKAGE's R=6371 km (not the IUGG "
          "6371.0088) -> 321.447478. Got " +
              fullPrec(sph));
    check(std::isfinite(km) && std::isfinite(sph) && std::fabs(sph - km) > 0.15,
          "AER-GRAPHX (iii): ...and the two answers DIFFER. Equal values here mean one spelling "
          "silently routes to the other's formula, which no single-value check can see. Vincenty " +
              fullPrec(km) + " vs sphere " + fullPrec(sph));

    // miles is a CONVERSION, not a label. A `miles` that returned kilometres
    // renders a plot with the right shape and a 61% understated axis.
    auto r3 = exec("geodist 37.5 127.0 35.1 129.0, miles\n", s);
    check(firstError(r3.blocks).is_null(),
          "AER-GRAPHX (iii): `miles` is accepted. Got `" + errText(firstError(r3.blocks)) + "`");
    const double mi = runR(s, "", "distance");
    check(std::isfinite(mi) && std::fabs(mi - 199.596022) < 0.07,
          "AER-GRAPHX (iii): ...and `miles` CONVERTS (x 0.621371192) -> 199.596022 statute "
          "miles. A handler that accepted the flag and returned kilometres answers " +
              fullPrec(km) + " here. Got " + fullPrec(mi));
    // BUG-426's discriminating arm, and the reason a value check alone is not
    // enough: the OLD handler accepted this spelling and wrote a constant
    // column, which looks like a working command. Stata answers r(198).
    auto r4 = exec("geodist 37.5 127.0 35.1 129.0, gen(dzz)\n", s);
    check(!firstError(r4.blocks).is_null() &&
              errText(firstError(r4.blocks)).find("immediate form") != std::string::npos,
          "AER-GRAPHX (iii): ...and generate() with four literals is REFUSED (Stata r(198)) -- the "
          "immediate form is a calculator, not a column writer. Got `" +
              errText(firstError(r4.blocks)) + "`");
  }

  // ── (iv) a missing coordinate gives a MISSING distance, never 0. ──────────
  // Zero is a legal distance (two identical points), so a handler that folded
  // missing to 0 puts a plausible number in the column and every downstream
  // mean is quietly wrong -- and `summarize` counts it, where a real missing
  // would have been dropped. Both directions are asserted: one missing AND no
  // zero.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec("clear\nset obs 2\n"
               "gen la1 = 37.5\n"
               "gen lo1 = 127.0\n"
               "gen la2 = 35.1\n"
               "gen lo2 = 129.0\n"
               "replace la2 = . in 2\n",
               s);
    auto r = exec("geodist la1 lo1 la2 lo2, gen(dm)\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-GRAPHX (iv): the VARIABLE form (four column names) runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    const double nMiss = countIf(s, "missing(dm)");
    const double nZero = countIf(s, "dm == 0");
    const double nGood = countIf(s, "dm > 321 & dm < 322");
    check(nMiss == 1,
          "AER-GRAPHX (iv): the row whose lat2 is missing gets a MISSING distance. Got " +
              fullPrec(nMiss) + " missing row(s)");
    check(nZero == 0,
          "AER-GRAPHX (iv): ...and NOT 0. Zero is a legal distance, so a missing folded to 0 is "
              "a plausible number that summarize then averages in. Got " + fullPrec(nZero) +
              " zero row(s)");
    check(nGood == 1,
          "AER-GRAPHX (iv): ...while the complete row still computes -- one bad row does not "
          "blank the column. Got " + fullPrec(nGood) + " good row(s)");
  }

  // ── (v) an out-of-range latitude is missing too, not a wrapped number. ────
  // |lat| > 90 has no point on the sphere. Vincenty's iteration still returns
  // SOMETHING for lat=95 (it wraps past the pole), so "it produced a number" is
  // exactly the failure mode: the plan's edge case says missing.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec("clear\nset obs 1\n"
               "gen bla = 95\n"
               "gen blo = 10\n"
               "gen bla2 = 35.1\n"
               "gen blo2 = 129.0\n",
               s);
    (void)exec("geodist bla blo bla2 blo2, gen(dbad)\n", s);
    const double nMiss = countIf(s, "missing(dbad)");
    check(nMiss == 1,
          "AER-GRAPHX (v): latitude 95 is off the sphere -> MISSING. The iteration happily "
          "returns a number for it, so a handler with no range check writes a confident wrong "
          "distance here. Got " + fullPrec(nMiss) + " missing row(s)");
  }

  // ── (vi) geonear picks the RIGHT neighbour, and names its columns. ────────
  // Three master points on the meridian at latitudes 0/10/20; four neighbours
  // half a degree north of each of the first three, plus a decoy at 40.
  // Hand-computed answer: 1->101, 2->102, 3->103, so the id column sums to 306.
  // The two wrong implementations this separates are the ones that look right:
  // "first row of the using file for everybody" sums to 303, and "last row"
  // sums to 416. The decoy at 40 exists so that a handler which simply took
  // the MAXIMUM distance is also caught.
  //
  // The distance band is deliberately WIDE (160..172 km against a Vincenty sum
  // of 165.9456 and a haversine sum of 166.7926). It is a UNIT check, not a
  // formula check: degrees would sum to 1.5, metres to 165946, miles to 103.1.
  // The handler declares the ellipsoid as geonear's default today, but Stata's
  // own geonear defaults to the SPHERE, so which one this row should ship is a
  // question the StataBE golden settles -- and a band that locked in today's
  // answer would turn red on the fix rather than on a defect. The formula IS
  // discriminated, in leg (iii), on geodist, where the contract is unambiguous.
  {
    piet::SessionState s(workdir);
    seed(s);
    spit(workdir + "/gnnb.csv",
         "nid,nlat,nlon\n101,0.5,0\n102,10.5,0\n103,20.5,0\n104,40,0\n");
    (void)exec("clear\nset obs 3\ngen mid = _n\ngen mlat = (_n-1)*10\ngen mlon = 0\n", s);
    auto r = exec("geonear mid mlat mlon using \"gnnb.csv\", neighbors(nid nlat nlon)\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-GRAPHX (vi): `geonear ... using \"gnnb.csv\", neighbors(nid nlat nlon)` runs "
          "clean -- the `using` file resolves through session.cwd. Got `" +
              errText(firstError(r.blocks)) + "`");
    const std::vector<std::string> vn = varNames(s);
    check(hasName(vn, "nid") && hasName(vn, "km_to_nid"),
          "AER-GRAPHX (vi): ...and it creates `nid` + `km_to_nid` -- the stem is the "
          "neighbors() ID VARIABLE name, and the distance column carries its unit in the "
          "prefix. Got: " + joinNames(vn));
    check(runR(s, "count", "N") == 3,
          "AER-GRAPHX (vi): ...and the master keeps its THREE rows. geonear appends columns, "
          "it does not join rows. Got " + fullPrec(runR(s, "count", "N")));
    const double idSum = sumOf(s, "nid");
    check(idSum == 306,
          "AER-GRAPHX (vi): ...and each master point gets its OWN nearest neighbour (101+102+"
          "103 = 306). 303 is 'the first using row for everybody', 416 is 'the last one', and "
          "both draw a perfectly ordinary-looking map. Got " + fullPrec(idSum));
    const double pair1 = countIf(s, "mid == 1 & nid == 101");
    const double pair3 = countIf(s, "mid == 3 & nid == 103");
    check(pair1 == 1 && pair3 == 1,
          "AER-GRAPHX (vi): ...and the PAIRING is right, not just the multiset -- a handler "
          "that assigned the correct three ids in the wrong ROW order sums to 306 too. Got "
          "rows matching (mid==1,nid==101): " + fullPrec(pair1) +
              ", (mid==3,nid==103): " + fullPrec(pair3));
    const double kmSum = sumOf(s, "km_to_nid");
    check(std::isfinite(kmSum) && kmSum > 160.0 && kmSum < 172.0,
          "AER-GRAPHX (vi): ...and the distances are in KILOMETRES (three half-degree meridian "
          "arcs sum to ~166). Degrees would sum to 1.5, metres to ~165946, miles to ~103. Got " +
              fullPrec(kmSum));
  }

  // ── (vii) genstub() renames the stem and `miles` renames the unit prefix. ─
  // The unit lives in the COLUMN NAME as well as in the number. A handler that
  // converted the value but kept the `km_to_` prefix hands the user a column
  // labelled kilometres holding miles -- the single most expensive kind of
  // quiet wrong answer in a geo pipeline -- and one that renamed the prefix
  // without converting is the same defect mirrored. Both halves are asserted.
  {
    piet::SessionState s(workdir);
    seed(s);
    spit(workdir + "/gnnb.csv",
         "nid,nlat,nlon\n101,0.5,0\n102,10.5,0\n103,20.5,0\n104,40,0\n");
    (void)exec("clear\nset obs 3\ngen mid = _n\ngen mlat = (_n-1)*10\ngen mlon = 0\n", s);
    auto r = exec("geonear mid mlat mlon using \"gnnb.csv\", neighbors(nid nlat nlon) "
                  "genstub(gn) miles\n",
                  s);
    check(firstError(r.blocks).is_null(),
          "AER-GRAPHX (vii): `genstub(gn) miles` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    const std::vector<std::string> vn = varNames(s);
    check(hasName(vn, "gn") && hasName(vn, "mi_to_gn"),
          "AER-GRAPHX (vii): ...and the columns are `gn` + `mi_to_gn` -- genstub() replaces the "
          "stem and `miles` replaces the unit prefix. Got: " + joinNames(vn));
    check(!hasName(vn, "km_to_gn"),
          "AER-GRAPHX (vii): ...and NOT `km_to_gn`: a column named kilometres holding miles is "
          "the wrong answer that survives every review. Got: " + joinNames(vn));
    const double miSum = sumOf(s, "mi_to_gn");
    check(std::isfinite(miSum) && miSum > 99.0 && miSum < 107.0,
          "AER-GRAPHX (vii): ...and the NUMBERS converted too (~103 miles against ~166 km). A "
          "rename with no conversion lands at ~166 here. Got " + fullPrec(miSum));
  }

  // ── (viii) the pair cap refuses BEFORE it writes anything. ────────────────
  // 25000 master x 2100 neighbours = 5.25e7 pairs, just over the 5e7 cap. Two
  // things are asserted and the second is the one that matters: a refusal that
  // has already appended a half-filled column leaves the frame in a state the
  // user then analyses, and the error they saw scrolls away. (The under-cap
  // control is leg (vi): geonear succeeding on a small frame is what makes
  // this leg a cap check rather than "geonear always refuses".)
  {
    piet::SessionState s(workdir);
    seed(s);
    std::string big = "nid,nlat,nlon\n";
    big.reserve(64 * 1024);
    for (int i = 0; i < 2100; ++i) {
      big += std::to_string(200000 + i);
      big += ",";
      big += fullPrec(0.001 * i);
      big += ",0\n";
    }
    spit(workdir + "/gnbig.csv", big);
    (void)exec("clear\nset obs 25000\ngen mid = _n\ngen mlat = _n/1000\ngen mlon = 0\n", s);
    const size_t before = varNames(s).size();
    auto r = exec("geonear mid mlat mlon using \"gnbig.csv\", neighbors(nid nlat nlon)\n", s);
    const nlohmann::ordered_json e = firstError(r.blocks);
    check(!e.is_null(),
          "AER-GRAPHX (viii): 25000 x 2100 = 5.25e7 pairs is over the build's 5e7 cap and is "
          "REFUSED. Silently grinding through it is a wall-clock cliff with no message.");
    check(has(errText(e), "5e7 pair cap"),
          "AER-GRAPHX (viii): ...and the sentence names the CAP, so the user knows to restrict "
          "the sample rather than to retry. Got `" + errText(e) + "`");
    check(rcOf(e) == "1",
          "AER-GRAPHX (viii): ...at rc 1 (engine_error): nothing about the syntax is wrong "
          "(198) and the command exists (199). Got rc " + rcOf(e));
    check(varNames(s).size() == before,
          "AER-GRAPHX (viii): ...and the frame gains NO column. A refusal that has already "
          "written half a column leaves the user analysing it after the message scrolls away. "
          "Had " + std::to_string(before) + ", now " + std::to_string(varNames(s).size()));
  }

  // ── (ix) cdfplot's last point is EXACTLY 1.0. ─────────────────────────────
  // The classic ECDF off-by-one ends at (n-1)/n = 0.8 or n/(n+1) = 0.8333 for
  // n=5, and both draw a curve that looks entirely right -- the eye cannot see
  // a top-of-axis gap of one fifth on an unlabelled plot. The equality is
  // asserted EXACTLY (not within a tolerance) because bundle F's `cumul` rule
  // is y = r/N with r = N on the last point, which is 1.0 in double arithmetic
  // by construction. The FIRST point is asserted too: 1/N = 0.2 separates the
  // i/N rule from a 0-based (i-1)/N that also ends below 1.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec("clear\nset obs 5\ngen cv = _n\n", s);
    auto r = exec("cdfplot cv\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-GRAPHX (ix): `cdfplot cv` runs clean. Got `" + errText(firstError(r.blocks)) + "`");
    const nlohmann::ordered_json blk = graphBlock(r.blocks);
    check(gstr(blk, "kind") == "twoway",
          "AER-GRAPHX (ix): ...and emits the TWOWAY block the renderer already draws -- this "
          "bundle adds no plot type. Got kind `" + gstr(blk, "kind") + "`");
    const nlohmann::ordered_json layers = garr(blk, "layers");
    check(layers.size() == 1,
          "AER-GRAPHX (ix): ...one layer with no by(). Got " + std::to_string(layers.size()));
    const nlohmann::ordered_json pts = layers.empty() ? nlohmann::ordered_json::array()
                                                      : garr(layers[0], "points");
    check(pts.size() == 5,
          "AER-GRAPHX (ix): ...with one point per observation (5). Got " +
              std::to_string(pts.size()));
    const double lastY = ptAt(pts, pts.empty() ? 0 : pts.size() - 1, 1);
    check(lastY == 1.0,
          "AER-GRAPHX (ix): ...and the LAST point's y is exactly 1.0. 0.8 is (n-1)/n and "
          "0.83333 is n/(n+1) -- the two classic off-by-ones, both of which draw a curve that "
          "looks correct. Got " + fullPrec(lastY));
    const double firstY = ptAt(pts, 0, 1);
    check(std::isfinite(firstY) && std::fabs(firstY - 0.2) < 1e-12,
          "AER-GRAPHX (ix): ...and the FIRST point's y is 1/N = 0.2, which is what separates "
          "the i/N rule from a 0-based (i-1)/N that also ends below 1. Got " + fullPrec(firstY));
    const double lastX = ptAt(pts, pts.empty() ? 0 : pts.size() - 1, 0);
    check(std::isfinite(lastX) && std::fabs(lastX - 5.0) < 1e-12,
          "AER-GRAPHX (ix): ...and x runs to the data's maximum (5), i.e. the points are the "
          "SORTED values and not row order. Got " + fullPrec(lastX));
  }

  // ── (x) by() gives one series PER GROUP, each ending at 1.0. ──────────────
  // A by() that parsed and was then dropped renders ONE pooled curve: same
  // block kind, same key set, rc 0, and a chart that answers a different
  // question. The per-group maximum x is the second half -- two layers whose
  // last x are both 6 would mean the split happened for the legend and not for
  // the data.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec("clear\nset obs 6\ngen cv = _n\ngen cg = 1\nreplace cg = 2 in 4/6\n", s);
    auto r = exec("cdfplot cv, by(cg)\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-GRAPHX (x): `cdfplot cv, by(cg)` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    const nlohmann::ordered_json blk = graphBlock(r.blocks);
    const nlohmann::ordered_json layers = garr(blk, "layers");
    check(layers.size() == 2,
          "AER-GRAPHX (x): ...and the series count equals the GROUP count (2). One layer here "
          "means by() was accepted and dropped, which renders a pooled ECDF at rc 0. Got " +
              std::to_string(layers.size()));
    if (layers.size() == 2) {
      const nlohmann::ordered_json p0 = garr(layers[0], "points");
      const nlohmann::ordered_json p1 = garr(layers[1], "points");
      check(p0.size() == 3 && p1.size() == 3,
            "AER-GRAPHX (x): ...each carrying its own group's three observations. Got " +
                std::to_string(p0.size()) + " and " + std::to_string(p1.size()));
      check(ptAt(p0, p0.empty() ? 0 : p0.size() - 1, 1) == 1.0 &&
                ptAt(p1, p1.empty() ? 0 : p1.size() - 1, 1) == 1.0,
            "AER-GRAPHX (x): ...and EACH group's ECDF reaches 1.0 -- a per-group curve, not a "
            "pooled one sliced in two (whose first slice would top out at 0.5). Got " +
                fullPrec(ptAt(p0, p0.empty() ? 0 : p0.size() - 1, 1)) + " and " +
                fullPrec(ptAt(p1, p1.empty() ? 0 : p1.size() - 1, 1)));
      check(std::fabs(ptAt(p0, p0.empty() ? 0 : p0.size() - 1, 0) - 3.0) < 1e-12 &&
                std::fabs(ptAt(p1, p1.empty() ? 0 : p1.size() - 1, 0) - 6.0) < 1e-12,
            "AER-GRAPHX (x): ...and the groups hold DIFFERENT data (max x 3 and 6, in "
            "ascending group order). Two layers with the same x range would mean the split "
            "reached the legend and not the points. Got " +
                fullPrec(ptAt(p0, p0.empty() ? 0 : p0.size() - 1, 0)) + " and " +
                fullPrec(ptAt(p1, p1.empty() ? 0 : p1.size() - 1, 0)));
    }
  }

  // ── (xi) grstyle's accept-note is pinned LITERALLY. ───────────────────────
  // The note IS the feature. An accept-note exists so the user is told that
  // their styling did not take effect; a silent no-op is the same behaviour
  // with the evidence removed, and it passes every rc check ever written. So
  // the assertion is the exact sentence, byte for byte, including the em dash
  // -- the same standard the extcompat gate message is held to.
  {
    const std::string kNote =
        "(grstyle: scheme styling is not applied in this build — accepted and ignored)";
    // No `use` anywhere below: grstyle is on kNoDataCommands, and `grstyle
    // init` is routinely the first line of a do-file.
    const char* kForms[] = {"grstyle init", "grstyle set color hue", "grstyle clear"};
    for (const char* form : kForms) {
      piet::SessionState s(workdir);
      seed(s);
      auto blocks = piet::dispatch(form, s);
      const nlohmann::ordered_json e = firstError(blocks);
      check(e.is_null(),
            std::string("AER-GRAPHX (xi): `") + form +
                "` is ACCEPTED (rc 0), not refused -- a refusal stops every do-file that opens "
                "with it, before any real work. Got `" + errText(e) + "`");
      const std::vector<std::string> d = displays(blocks);
      check(!d.empty() && d[0] == kNote,
            std::string("AER-GRAPHX (xi): ...and it SAYS SO, byte for byte. A silent no-op is "
                        "this same behaviour with the evidence removed and passes every return-"
                        "code check. Expected `") +
                kNote + "`, got `" + (d.empty() ? std::string("<no display block>") : d[0]) + "`");
    }
  }

  // ── (xii) event_plot keeps the stub coefficients, in RELATIVE-TIME order. ─
  // The regressors are handed to `regress` in a DELIBERATELY SCRAMBLED order
  // (L1, F2, ctrl, L0, L2, F1) so that this leg separates three different
  // wrong forwards, all of which return a coefplot block at rc 0:
  //   - no filtering at all      -> 7 coefs including ctrl and _cons;
  //   - filtering, no reordering -> the 5 right coefs in coeftable order
  //                                 (L1ev F2ev L0ev L2ev F1ev), which plots a
  //                                 sawtooth "event study" whose x axis is not
  //                                 time at all;
  //   - the contract              -> F2ev F1ev L0ev L1ev L2ev.
  // The scramble is what makes the ORDER check able to fail; with the natural
  // declaration order the two middle cases are indistinguishable.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec("clear\n"
               "set obs 12\n"
               "gen F2ev = (_n <= 2)\n"
               "gen F1ev = (_n > 2 & _n <= 4)\n"
               "gen L0ev = (_n > 4 & _n <= 6)\n"
               "gen L1ev = (_n > 6 & _n <= 8)\n"
               "gen L2ev = (_n > 8 & _n <= 10)\n"
               "gen ctrl = _n\n"
               "gen ey = _n*_n\n"
               "regress ey L1ev F2ev ctrl L0ev L2ev F1ev\n"
               "eststo ev1\n",
               s);
    auto r = exec("event_plot ev1, stub_lag(L#ev) stub_lead(F#ev)\n", s);
    check(firstError(r.blocks).is_null(),
          "AER-GRAPHX (xii): `event_plot ev1, stub_lag(L#ev) stub_lead(F#ev)` runs clean. Got `" +
              errText(firstError(r.blocks)) + "`");
    const nlohmann::ordered_json blk = graphBlock(r.blocks);
    check(gstr(blk, "kind") == "coefplot",
          "AER-GRAPHX (xii): ...and forwards to the coefplot block (this row is a shim over "
          "runCoefplot, not a new renderer). Got kind `" + gstr(blk, "kind") + "`");
    const nlohmann::ordered_json models = garr(blk, "models");
    check(models.size() == 1,
          "AER-GRAPHX (xii): ...for the ONE named model. Got " + std::to_string(models.size()));
    std::vector<std::string> got;
    if (!models.empty())
      for (const auto& c : garr(models[0], "coefs")) got.push_back(gstr(c, "name"));
    check(got.size() == 5,
          "AER-GRAPHX (xii): ...and exactly the FIVE stub matches survive. 7 means the stub "
          "filter never ran and `ctrl` and `_cons` are being plotted as event-time "
          "coefficients. Got " + std::to_string(got.size()) + ": " + joinNames(got));
    check(joinNames(got) == "F2ev F1ev L0ev L1ev L2ev",
          "AER-GRAPHX (xii): ...in RELATIVE-TIME order -- leads most-negative first, then lags. "
          "The regressors were declared scrambled on purpose, so a forward that filtered but "
          "did not reorder answers `L1ev F2ev L0ev L2ev F1ev` here: a plausible plot of the "
          "right coefficients against the wrong axis. Got: " + joinNames(got));
  }

  // ── (xiii) event_plot's two refusals say which half of the line is wrong. ─
  // Both matter because both silently degrade otherwise: a stub that matches
  // nothing must not fall back to "plot every coefficient" (which is a chart of
  // the controls), and omitting the stubs entirely must not be read as "plot
  // everything" either.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec("clear\n"
               "set obs 12\n"
               "gen F1ev = (_n <= 4)\n"
               "gen L0ev = (_n > 4 & _n <= 8)\n"
               "gen ctrl = _n\n"
               "gen ey = _n*_n\n"
               "regress ey F1ev L0ev ctrl\n"
               "eststo ev2\n",
               s);
    auto r = exec("event_plot ev2, stub_lag(zz#)\n", s);
    const nlohmann::ordered_json e = firstError(r.blocks);
    check(!e.is_null(),
          "AER-GRAPHX (xiii): a stub matching NO coefficient is refused, not quietly widened to "
          "'plot them all' -- which would render the controls as an event study.");
    check(has(errText(e), "no coefficients match"),
          "AER-GRAPHX (xiii): ...and the sentence says the stubs matched nothing, which is the "
          "half of the line the user has to fix. Got `" + errText(e) + "`");
    auto r2 = exec("event_plot ev2\n", s);
    const nlohmann::ordered_json e2 = firstError(r2.blocks);
    check(!e2.is_null() && errField(e2, "code") == "syntax_error" && rcOf(e2) == "198",
          "AER-GRAPHX (xiii): ...and event_plot with NEITHER stub option is a syntax error "
          "r(198), not a silent forward to a plain coefplot. Got code `" +
              errField(e2, "code") + "` rc " + rcOf(e2));
  }

  // ── (xiv) grstyle / grc1leg2 / event_plot run with NO dataset loaded. ─────
  // BUG-343's class, and the reason bundle D exists: the registry row and the
  // dispatcher arm can both be right and the command still answers "No dataset
  // is loaded" because its name is missing from kNoDataCommands. `grstyle
  // init` is a preamble line; grc1leg2 and event_plot read named graphs and
  // stored estimates, which outlive the `clear` in front of them. Only the
  // no_data_loaded code is under test -- these two legitimately erroring for
  // want of a graph or an estimate is fine here.
  {
    const char* kProbes[] = {"grstyle init", "grc1leg2 g1 g2", "event_plot"};
    for (const char* probe : kProbes) {
      piet::SessionState s(workdir);  // deliberately NO use/sysuse
      seed(s);
      auto blocks = piet::dispatch(probe, s);
      bool sawNoData = false;
      for (const auto& b : blocks)
        if (b.is_object() && b.value("code", std::string()) == "no_data_loaded") sawNoData = true;
      check(!sawNoData, std::string("AER-GRAPHX (xiv): `") + probe +
                            "` runs with no dataset loaded (kNoDataCommands)");
    }
  }

  // ── (xv) the UNREGISTERED names answer a generic r(199) and nothing more. ─
  // These are asserted at the generic shape only: a check pinning a specific
  // `reason` string for a name with no registry row would be asserting a
  // promise this tree does not make.
  //
  // `grc1leg` USED TO HEAD THIS LIST, and the arm below it is where it went.
  // The reason it was here was that "its package states no author anywhere --
  // not in the ado, not in the help file", and that is still true of both of
  // those files; what changed is that a third place was found (the installer's
  // own record of the .pkg description) and it names Vince Wiggins. A refusal
  // whose stated ground was "unmeasured" is INVERTED by the measurement, not
  // deleted -- so the protection this arm gave (nobody ships an unattributed
  // community command at rc 0) is re-keyed onto the attribution itself rather
  // than dropped along with the name.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec(kThreeGraphs, s);
    const char* kUnregistered[] = {"cibar gy", "spmap", "binsreg gy gx",
                                   "maptile gy",    "shp2dta",  "geoinpoly", "heatplot gy",
                                   "distplot gy"};
    for (const char* cmd : kUnregistered) {
      auto blocks = piet::dispatch(cmd, s);
      const nlohmann::ordered_json e = firstError(blocks);
      check(!e.is_null() && errField(e, "code") == "unsupported_command" && rcOf(e) == "199",
            std::string("AER-GRAPHX (xv): `") + cmd +
                "` answers the GENERIC unsupported-command r(199) -- it has no registry row, "
                "because its package claims no author. Got code `" + errField(e, "code") +
                "` rc " + rcOf(e));
      check(graphBlock(blocks).is_null(),
            std::string("AER-GRAPHX (xv): ...and `") + cmd +
                "` produces NO graph block.");
    }
  }

  // ── (xv-b) `grc1leg` is the INVERSION of the arm above, and it asserts the
  // same thing from the other side: no unattributed community command ships at
  // rc 0. It now answers, so the two halves of that promise are (1) it really
  // does draw, and (2) the registry really does carry the author -- checked
  // here together, because either one alone is the failure this guards.
  {
    piet::SessionState s(workdir);
    seed(s);
    (void)exec(kThreeGraphs, s);
    auto blocks = piet::dispatch("grc1leg g1 g2", s);
    check(firstError(blocks).is_null() && !graphBlock(blocks).is_null(),
          "AER-GRAPHX (xv-b): `grc1leg g1 g2` now DRAWS -- Vince Wiggins' original answered by "
          "Mead Over's grc1leg2 rewrite, an ACCEPTED-NAMED substitution");
    const piet::registry::CommunityName* cn = piet::registry::findCommunityName("grc1leg");
    const std::string author = cn == nullptr ? std::string() : cn->author;
    check(cn != nullptr && author.find("Wiggins") != std::string::npos,
          "AER-GRAPHX (xv-b): ...and its registry row NAMES the author, which is the whole "
          "reason the un-suffixed spelling was refused until today. Got `" + author + "`");
  }

  // ── (xvi) the community-name gate reaches this bundle. ────────────────────
  // Pattern and ISOLATION read from testAerGtoolsBundle's identical leg: `set
  // extcompat off` moves PERSISTED settings, and a gate that writes the
  // operator's real settings file is the BUG-346 class, so the store is
  // pointed at this function's own workdir and the previous value is RESTORED
  // rather than unset (the harness pins PIET_SETTINGS_FILE on some runs).
  // POSIX-only, and the skip is named rather than left to be discovered: the
  // isolation needs setenv/unsetenv. What is lost on Windows is this bundle's
  // MEMBERSHIP in the gate; testExtcompatGate still covers the mechanism.
#ifndef _WIN32
  {
    const char* prevSettings = std::getenv("PIET_SETTINGS_FILE");
    const std::string savedSettings = prevSettings ? prevSettings : "";
    const std::string ownSettings = (fs::path(workdir) / "settings.json").u8string();
    setenv("PIET_SETTINGS_FILE", ownSettings.c_str(), 1);

    piet::SessionState s(workdir);
    seed(s);
    (void)exec("clear\nset obs 5\ngen cv = _n\n", s);
    (void)piet::dispatch("set extcompat off", s);
    auto blocks = piet::dispatch("cdfplot cv", s);
    const nlohmann::ordered_json e = firstError(blocks);
    check(!e.is_null() && errField(e, "code") == "unsupported_command" &&
              errField(e, "token") == "cdfplot",
          "AER-GRAPHX (xvi): a bundle-J name is gated exactly as `reghdfe` is -- all six rows "
          "are community spellings and the opt-out has to reach them, or the setting promises "
          "an isolation it does not deliver. Got code `" + errField(e, "code") + "` token `" +
              errField(e, "token") + "`");
    check(!e.is_null() && rcOf(e) == "199",
          "AER-GRAPHX (xvi): ...and the gate maps to r(199). Got rc " + rcOf(e));

    // Restore ORDER matters: env var first, then invalidate the cache, then
    // force the process default. Invalidating after the set would re-resolve
    // the flag from the restored store and silently undo the restore.
    if (!savedSettings.empty()) {
      setenv("PIET_SETTINGS_FILE", savedSettings.c_str(), 1);
    } else {
      unsetenv("PIET_SETTINGS_FILE");
    }
    piet::settings::invalidateExtcompatCache();
    piet::settings::setExtcompatEnabled(true);
    // Verified by DISPATCHING the name again, not by reading the flag back:
    // the flag was written by the line directly above, so reading it proves
    // the setter works and nothing about the gate. The cases after this one
    // inherit whatever this leg leaves behind.
    auto again = piet::dispatch("cdfplot cv", s);
    const nlohmann::ordered_json againErr = firstError(again);
    check(againErr.is_null() || errField(againErr, "code") != "unsupported_command",
          "AER-GRAPHX (xvi): ...and once the default is restored the name ANSWERS again, so "
          "this function hands the following cases the state they were written against.");
  }
#endif

  fs::remove_all(workdir, ec);
}


// BETA campaign Phase B (B-graph part A cluster): title()/subtitle()/
// xtitle()/ytitle() dequote + glued-suboption strip (BS-008/U1), note()
// wiring (BS-009/U2), bare-form name() -> namedGraphs registration
// (BS-010/U3), and `graph combine ..., name(X, replace)` suffix-leak fix
// (BS-057). All four share ONE mechanism (BS-099's "single title/axis/
// legend/name/note consumer") added in graph_m4.cpp.
void testBS008To057GraphTitleNoteName() {
  std::printf("[selftest] --- BS-008/009/010/057 graph title/note/name (B-graph part A) ---\n");
  namespace fs = std::filesystem;
  std::string base = (fs::temp_directory_path() / "piet_native_selftest_bs008graph").u8string();
  std::error_code ec;
  fs::remove_all(base, ec);
  fs::create_directories(base, ec);

  auto runCmd = [](piet::SessionState& session, const std::string& cmd) {
    nlohmann::json body{{"cmd", cmd}};
    auto r = piet::routes::command(session, body.dump());
    return nlohmann::json::parse(r.json);
  };
  auto firstBlockOfType = [](const nlohmann::json& j, const std::string& type) -> nlohmann::json {
    for (const auto& b : j.value("blocks", nlohmann::json::array())) {
      if (b.value("type", std::string()) == type) return b;
    }
    return nlohmann::json();
  };

  // Every leg below builds its graphs from real `fatal`/`beertax` columns
  // and chains firstBlockOfType(...).value(...) unguarded. In the isolated/
  // subset-build environment (no piet/packaging/sample/fatalities.csv), that
  // throws the same json.exception.type_error.306 as the earlier BUG-039/
  // BUG-055/BUG-056/BS-013/BS-048/BS-047 legs (FIX-SELFTEST-ISOBUILD-CRASH).
  // One up-front probe covers all five legs below (a single skip() call,
  // same as the block-level skips elsewhere in this file) instead of
  // guarding each block individually.
  {
    std::string probeWd = base + "/_fixture_probe";
    fs::create_directories(probeWd, ec);
    piet::SessionState probeSession(probeWd);
    runCmd(probeSession, "sysuse fatalities, clear");
    if (!probeSession.hasData()) {
      skip("BS-008/009/010/057 graph title/note/name (all 5 legs)",
           "`sysuse fatalities` setup returned no data");
      fs::remove_all(base, ec);
      return;
    }
  }

  // --- BS-008 (U1): title()/xtitle()/ytitle() dequote + glued sub-option
  // strip -- the idiomatic quoted-title-with-a-suboption shape analysis.do
  // actually uses (e.g. `title("`depvar_label'", size(medsmall))`).
  {
    std::string wd = base + "/title";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    runCmd(session, "sysuse fatalities, clear");
    auto j = runCmd(session,
        R"DO(scatter fatal beertax, title("Fatalities vs Beertax", size(large)) xtitle("Beer tax ($)") ytitle("Traffic fatalities"))DO");
    auto g = firstBlockOfType(j, "graph");
    check(g.value("title", std::string()) == "Fatalities vs Beertax",
          "BS-008: quoted title() with a glued size() suboption dequotes to clean text (got \"" +
              g.value("title", std::string()) + "\")");
    check(g.value("title", std::string()).find('"') == std::string::npos,
          "BS-008: dequoted title carries no literal quote characters");
    check(g.value("x_label", std::string()) == "Beer tax ($)",
          "BS-008: quoted xtitle() dequotes to clean text (got \"" + g.value("x_label", std::string()) +
              "\")");
    check(g.value("y_label", std::string()) == "Traffic fatalities",
          "BS-008: quoted ytitle() dequotes to clean text (got \"" + g.value("y_label", std::string()) +
              "\")");
  }

  // --- BS-008 regression guard: an UNQUOTED bare title with a glued
  // suboption also gets the suboption stripped (not just the quoted case).
  {
    std::string wd = base + "/title_bare";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    runCmd(session, "sysuse fatalities, clear");
    auto j = runCmd(session, "scatter fatal beertax, title(Beertax, size(large))");
    auto g = firstBlockOfType(j, "graph");
    check(g.value("title", std::string()) == "Beertax",
          "BS-008: an unquoted bare title(Beertax, size(large)) still strips the glued "
          "suboption (got \"" +
              g.value("title", std::string()) + "\")");
  }

  // --- BS-009 (U2): note() reaches the block, dequoted; absent note() ->
  // present-but-empty (never dropped, never errors).
  {
    std::string wd = base + "/note";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    runCmd(session, "sysuse fatalities, clear");
    auto j = runCmd(session, R"(scatter fatal beertax, note("Source: test fixture."))");
    auto g = firstBlockOfType(j, "graph");
    check(g.contains("note"),
          "BS-009: the graph block now carries a `note` key at all (previously absent entirely)");
    check(g.value("note", std::string()) == "Source: test fixture.",
          "BS-009: note() text reaches the block, dequoted (got \"" + g.value("note", std::string()) +
              "\")");

    auto j2 = runCmd(session, "scatter fatal beertax");
    auto g2 = firstBlockOfType(j2, "graph");
    check(g2.contains("note") && g2.value("note", std::string()).empty(),
          "BS-009 regression guard: a graph with no note() still carries a `note` key, empty by "
          "default");
  }

  // --- BS-010 (U3): bare-form name() registers into namedGraphs (mirrors
  // the already-working parenthesised twoway path at dispatcher.cpp).
  {
    std::string wd = base + "/name";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    runCmd(session, "sysuse fatalities, clear");
    check(session.namedGraphs.empty(), "BS-010 setup: namedGraphs starts empty");

    runCmd(session, "scatter fatal beertax, name(scatterG)");
    check(session.namedGraphs.count("scatterG") == 1,
          "BS-010: bare `scatter ..., name(scatterG)` registers scatterG in namedGraphs "
          "(previously bare forms never registered at all)");

    runCmd(session, "histogram fatal, name(histG, replace)");
    check(session.namedGraphs.count("histG") == 1,
          "BS-010: bare `histogram ..., name(histG, replace)` registers the BARE name "
          "(glued `, replace` suboption stripped, same rule as the twoway path)");
    check(session.namedGraphs.count("histG, replace") == 0,
          "BS-010: the raw \"histG, replace\" string is NOT used as the stored key");

    runCmd(session, "kdensity fatal, name(kdensG)");
    check(session.namedGraphs.count("kdensG") == 1, "BS-010: bare `kdensity ..., name(kdensG)` registers");

    runCmd(session, "line fatal beertax, name(lineG)");
    check(session.namedGraphs.count("lineG") == 1, "BS-010: bare `line ..., name(lineG)` registers");

    // xtset ALSO seeds session.timevar (handlers_m3.cpp::runXtset), so one
    // call unblocks both xtline (needs panelvar+timevar) and tsline (needs
    // only timevar) -- no separate tsset needed on this multi-state panel.
    // The panel id is numeric: xtset refuses the string `state` (BUG-1164).
    runCmd(session, "egen sid = group(state)");
    runCmd(session, "xtset sid year");
    runCmd(session, "xtline fatal, name(xtlineG)");
    check(session.namedGraphs.count("xtlineG") == 1, "BS-010: bare `xtline ..., name(xtlineG)` registers");
    runCmd(session, "tsline fatal, name(tslineG)");
    check(session.namedGraphs.count("tslineG") == 1, "BS-010: bare `tsline ..., name(tslineG)` registers");

    // End-to-end proof: a bare-form-registered graph can actually be
    // combined -- the whole point of the named-panels -> `graph combine`
    // idiom this fix unblocks (previously `graph combine` honestly
    // rejected with "no graph named 'scatterG'" for every one of these).
    auto jc = runCmd(session, "graph combine scatterG histG");
    auto errC = firstBlockOfType(jc, "error");
    check(errC.is_null(),
          "BS-010: `graph combine scatterG histG` succeeds (both bare-form names resolve)");
  }

  // --- BS-057: `graph combine ..., name(X, replace)` -- stored key has no
  // suffix (previously "combined_X, replace" was baked in verbatim).
  {
    std::string wd = base + "/combine_name";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    runCmd(session, "sysuse fatalities, clear");
    runCmd(session, "scatter fatal beertax, name(panelA)");
    runCmd(session, "histogram fatal, name(panelB)");
    auto j = runCmd(session, "graph combine panelA panelB, name(combined_X, replace)");
    auto errC = firstBlockOfType(j, "error");
    check(errC.is_null(), "BS-057 setup: `graph combine panelA panelB, name(combined_X, replace)` succeeds");
    check(session.namedGraphs.count("combined_X") == 1,
          "BS-057: the stored namedGraphs key is the bare \"combined_X\" (suffix stripped)");
    check(session.namedGraphs.count("combined_X, replace") == 0,
          "BS-057: the literal \"combined_X, replace\" string is NOT baked into the stored key "
          "(previously it was)");
    auto gc = firstBlockOfType(j, "graph");
    check(gc.value("name", std::string()) == "combined_X",
          "BS-057: the returned block's own `name` field is also the bare \"combined_X\", not "
          "\"combined_X, replace\"");

    // Regression guard: `, replace` genuinely replaces (re-running under
    // the same cleaned name overwrites in place, doesn't accumulate a
    // second, differently-keyed entry).
    size_t countBefore = session.namedGraphs.size();
    runCmd(session, "graph combine panelA panelB, name(combined_X, replace)");
    check(session.namedGraphs.size() == countBefore,
          "BS-057 regression guard: re-combining under the same cleaned name replaces in place "
          "(map size unchanged), does not accumulate duplicate suffixed keys");
  }

  fs::remove_all(base, ec);
}


// BETA campaign Phase B (B-graph part B cluster): `graph combine` on-disk
// `.gph` file-path resolution (BS-007/D3), coefplot/marginsplot ->
// lastGraph/namedGraphs registration (BS-011), `graph export` `as(fmt)`
// parse + honest PNG/PDF-fallback wording (BS-012/D8).
void testBS007To012GraphCombineExportRegister() {
  std::printf(
      "[selftest] --- BS-007/011/012 graph combine .gph + coefplot/marginsplot register + "
      "export as() (B-graph part B) ---\n");
  namespace fs = std::filesystem;
  std::string base = (fs::temp_directory_path() / "piet_native_selftest_bs007graphb").u8string();
  std::error_code ec;
  fs::remove_all(base, ec);
  fs::create_directories(base, ec);

  auto runCmd = [](piet::SessionState& session, const std::string& cmd) {
    nlohmann::json body{{"cmd", cmd}};
    auto r = piet::routes::command(session, body.dump());
    return nlohmann::json::parse(r.json);
  };
  auto firstBlockOfType = [](const nlohmann::json& j, const std::string& type) -> nlohmann::json {
    for (const auto& b : j.value("blocks", nlohmann::json::array())) {
      if (b.value("type", std::string()) == type) return b;
    }
    return nlohmann::json();
  };
  auto readFile = [](const std::string& path) -> std::string {
    std::ifstream in(cecon::to_native_path(path), std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
  };

  // --- BS-007 (D3): `graph save NAME "file.gph"` writes a real spec-JSON
  // file (never a raster/frozen-pixel format); `graph combine "a.gph"
  // "b.gph"` (quoted FILE-PATH args, not names) loads and re-renders them.
  // Mirrors analysis.do's actual loop-then-combine idiom: `graph save` a
  // DIFFERENT tempfile each iteration while reusing the SAME in-memory
  // name -- so only the on-disk files carry each iteration's distinct
  // graph by the time `graph combine` runs (analysis.do:1533's
  // `graph_list` accumulates quoted tempfile paths, not names).
  {
    std::string wd = base + "/combine_gph";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    runCmd(session, "sysuse fatalities, clear");
    if (!session.hasData()) {
      // Same isolated/subset-build fixture gap as the earlier BUG-039/
      // BUG-055/BUG-056/BS-013/BS-048/BS-047/BS-008..057 legs above
      // (FIX-SELFTEST-ISOBUILD-CRASH): without fatalities.csv, `scatter`/
      // `histogram` never produce real graph blocks and the unguarded
      // .value() calls below would crash. Skip the whole .gph-combine leg.
      skip("BS-007 .gph-combine leg", "`sysuse fatalities` setup returned no data");
    } else {
      std::string p1 = wd + "/panel1.gph";
      std::string p2 = wd + "/panel2.gph";

      runCmd(session, "scatter fatal beertax, name(loopG)");
      auto jSave1 = runCmd(session, "graph save loopG \"" + p1 + "\", replace");
      check(firstBlockOfType(jSave1, "error").is_null(),
            "BS-007 setup: `graph save loopG \"panel1.gph\"` succeeds");
      check(fs::exists(p1, ec), "BS-007: `graph save` writes a real file to disk at the given path");

      std::string content1 = readFile(p1);
      check(content1.find("\"_magic\":\"PIETGPH1\"") != std::string::npos,
            "BS-007 (D3): the saved .gph file is the spec-JSON envelope (PIETGPH1 magic), never a "
            "raster/frozen-pixel format");
      check(content1.find("\"kind\":\"scatter\"") != std::string::npos,
            "BS-007: the saved .gph's embedded block is the actual scatter spec");

      // Same in-memory name, DIFFERENT graph, saved to a DIFFERENT file.
      // serset-twoway SS: redrawing onto a name in memory needs `replace` --
      // without it Stata answers `graph loopG already exists`, r(110)
      // (w2al_twoway_family.log:5024-5029 tf_st_dup2), so the loop idiom this
      // leg models writes name(loopG, replace).
      runCmd(session, "histogram fatal, name(loopG, replace)");
      auto jSave2 = runCmd(session, "graph save loopG \"" + p2 + "\", replace");
      check(firstBlockOfType(jSave2, "error").is_null(),
            "BS-007 setup: `graph save loopG \"panel2.gph\"` (2nd iteration, same name) succeeds");
      check(session.namedGraphs.count("loopG") == 1 &&
                session.namedGraphs.at("loopG").value("kind", std::string()) == "histogram",
            "BS-007 setup: the in-memory \"loopG\" now holds only the LAST iteration (histogram) -- "
            "proving a name-only combine could not have recovered the scatter panel");

      // `graph combine` with QUOTED FILE-PATH args (not names) -- BS-007's
      // actual fix. Previously this was a hard failure: "no graph named
      // '\"panel1.gph\"'" (the literal quoted string as a namedGraphs key,
      // since the tokenizer is not quote-aware and nothing tried a file load).
      auto jCombine = runCmd(session, "graph combine \"" + p1 + "\" \"" + p2 + "\"");
      auto errCombine = firstBlockOfType(jCombine, "error");
      check(errCombine.is_null(),
            "BS-007: `graph combine \"panel1.gph\" \"panel2.gph\"` (on-disk paths) succeeds (was a "
            "hard failure before)");
      auto gCombine = firstBlockOfType(jCombine, "graph");
      check(gCombine.value("kind", std::string()) == "graph_combine",
            "BS-007: the combine result is a graph_combine block");
      auto panels = gCombine.value("panels", nlohmann::json::array());
      check(panels.size() == 2, "BS-007: the combine has both panels (2), not 0/1");
      if (panels.size() == 2) {
        check(panels[0].value("kind", std::string()) == "scatter",
              "BS-007: panel 1 (loaded from panel1.gph) is the ORIGINAL scatter, not the "
              "later-overwritten in-memory histogram");
        check(panels[1].value("kind", std::string()) == "histogram",
              "BS-007: panel 2 (loaded from panel2.gph) is the histogram");
      }

      // Regression guard: a plain bare NAME still resolves via namedGraphs
      // (the pre-existing behavior is untouched by the new file-load branch).
      auto jCombineName = runCmd(session, "graph combine loopG loopG");
      check(firstBlockOfType(jCombineName, "error").is_null(),
            "BS-007 regression guard: bare-name `graph combine loopG loopG` (no file paths) still "
            "resolves via session.namedGraphs as before");

      // Honest reject: a token that resolves as NEITHER an in-memory graph
      // NOR a readable .gph file.
      auto jBad = runCmd(session, "graph combine nosuchgraph123");
      auto errBad = firstBlockOfType(jBad, "error");
      check(!errBad.is_null(),
            "BS-007: `graph combine` on an unresolvable token (no name, no file) is an honest "
            "error, not a crash");
    }
  }

  // --- BS-011: coefplot/marginsplot populate session.lastGraph + (when
  // name() is given) session.namedGraphs, mirroring scatter/twoway. Without
  // this, `graph export`/`graph save`/`graph combine` unconditionally
  // failed with "no graph in memory" immediately after either command.
  {
    std::string wd = base + "/coefplot_register";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    runCmd(session, "sysuse fatalities, clear");
    if (!session.hasData()) {
      // Same isolated/subset-build fixture gap as BS-007 above
      // (FIX-SELFTEST-ISOBUILD-CRASH): without fatalities.csv, `regress`/
      // `coefplot`/`logit`/`marginsplot` never produce real blocks and the
      // unguarded .value() calls below would crash. Skip the whole
      // coefplot/marginsplot-register leg.
      skip("BS-011 coefplot/marginsplot-register leg", "`sysuse fatalities` setup returned no data");
    } else {
      check(!session.lastGraph.has_value(), "BS-011 setup: lastGraph starts unset");

      runCmd(session, "regress fatal beertax unemp");
      auto jCoef = runCmd(session, "coefplot");
      check(firstBlockOfType(jCoef, "error").is_null(), "BS-011 setup: bare `coefplot` succeeds");
      check(session.lastGraph.has_value() && session.lastGraph->value("kind", std::string()) == "coefplot",
            "BS-011: `coefplot` now populates session.lastGraph (previously never set at all)");

      // The actual proof: export succeeds right after coefplot (was an
      // honest "no graph in memory" failure before this fix).
      std::string coefSvg = wd + "/coef.svg";
      auto jExpCoef = runCmd(session, "graph export \"" + coefSvg + "\", replace");
      check(firstBlockOfType(jExpCoef, "error").is_null(),
            "BS-011: `graph export` succeeds immediately after `coefplot` (was \"no graph in "
            "memory\" before this fix)");
      check(fs::exists(coefSvg, ec), "BS-011: the coefplot export actually wrote a file");
      check(readFile(coefSvg).rfind("<svg", 0) == 0,
            "BS-011: the coefplot export is a real SVG document (starts with <svg -- proves "
            "buildSvgDispatch has a coefplot branch, not a crash/garbage render)");

      // name() registration also works for coefplot.
      runCmd(session, "coefplot, name(coefG)");
      check(session.namedGraphs.count("coefG") == 1,
            "BS-011: `coefplot ..., name(coefG)` registers into namedGraphs same as scatter/twoway");

      // marginsplot: same proof, after logit + margins.
      runCmd(session, "gen ybin = spirits > 2");
      runCmd(session, "logit ybin unemp");
      runCmd(session, "margins, dydx(*)");
      auto jMp = runCmd(session, "marginsplot");
      check(firstBlockOfType(jMp, "error").is_null(), "BS-011 setup: bare `marginsplot` succeeds");
      check(session.lastGraph.has_value() && session.lastGraph->value("kind", std::string()) == "marginsplot",
            "BS-011: `marginsplot` now populates session.lastGraph (previously never set at all)");

      std::string mpSvg = wd + "/mp.svg";
      auto jExpMp = runCmd(session, "graph export \"" + mpSvg + "\", replace");
      check(firstBlockOfType(jExpMp, "error").is_null(),
            "BS-011: `graph export` succeeds immediately after `marginsplot` (was \"no graph in "
            "memory\" before this fix)");
      check(fs::exists(mpSvg, ec), "BS-011: the marginsplot export actually wrote a file");
      check(readFile(mpSvg).rfind("<svg", 0) == 0,
            "BS-011: the marginsplot export is a real SVG document (buildSvgDispatch has a "
            "marginsplot branch)");

      runCmd(session, "marginsplot, name(mpG)");
      check(session.namedGraphs.count("mpG") == 1,
            "BS-011: `marginsplot ..., name(mpG)` registers into namedGraphs same as scatter/twoway");

      // End-to-end: the two newly-registered named graphs (different kinds)
      // can be combined together.
      auto jCombineBoth = runCmd(session, "graph combine coefG mpG");
      check(firstBlockOfType(jCombineBoth, "error").is_null(),
            "BS-011: coefplot+marginsplot named graphs can be `graph combine`d together");
    }
  }

  // --- L2-FIX-B (BUG-066 / C3-F5): LIVE-VIEW point decimation. A big scatter
  // is thinned for the console/bridge block (bounded DOM + bridge copy) with an
  // honest n_shown_points/n_total_points tag, while session.lastGraph -- what
  // `graph export`/`save`/`combine` read -- keeps EVERY point (golden-pinned,
  // no re-pin). A sub-threshold graph is left completely untouched.
  {
    std::string wd = base + "/liveview_decimation";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);

    // 6000 finite points > the 5000-point view budget -> must decimate.
    runCmd(session, "set obs 6000");
    runCmd(session, "gen x = _n");
    runCmd(session, "gen y = _n * 2");
    auto jBig = runCmd(session, "scatter y x");
    auto gBig = firstBlockOfType(jBig, "graph");
    check(gBig.value("kind", std::string()) == "scatter",
          "L2-FIX-B setup: 6000-row `scatter y x` renders a scatter graph block");

    const size_t viewBudget = 5000;
    check(gBig.contains("n_total_points") && gBig.contains("n_shown_points"),
          "L2-FIX-B: an over-budget scatter is tagged with n_total_points/n_shown_points");
    size_t nTotal = gBig.value("n_total_points", size_t{0});
    size_t nShown = gBig.value("n_shown_points", size_t{0});
    check(nTotal == 6000,
          "L2-FIX-B: n_total_points reports the TRUE plotted-point count (6000), not the thinned one");
    check(nShown < nTotal && nShown <= viewBudget,
          "L2-FIX-B: the live view is thinned below the view budget (n_shown < n_total, <= 5000)");
    check(gBig["points"].is_array() && gBig["points"].size() == nShown,
          "L2-FIX-B: the live-view block's points array size equals the reported n_shown_points");

    // The FULL block is what `graph export` reads -- session.lastGraph must
    // still hold all 6000 points and carry NO thinning tag.
    check(session.lastGraph.has_value(),
          "L2-FIX-B: session.lastGraph is populated after the scatter");
    check(session.lastGraph->value("points", nlohmann::ordered_json::array()).size() == 6000,
          "L2-FIX-B: session.lastGraph (the `graph export` source) keeps ALL 6000 points -- live-view "
          "decimation never touches the export path");
    check(!session.lastGraph->contains("n_shown_points"),
          "L2-FIX-B: the cached full block is NOT tagged as decimated (only the live-view copy is)");

    // A `graph export` off that full block succeeds and writes real SVG (the
    // golden-pinned path is reached with the untouched point set).
    std::string svgOut = wd + "/full_export.svg";
    auto jExp = runCmd(session, "graph export \"" + svgOut + "\", replace");
    check(firstBlockOfType(jExp, "error").is_null(),
          "L2-FIX-B: `graph export` after a decimated live view still succeeds");
    check(fs::exists(svgOut, ec) && readFile(svgOut).rfind("<svg", 0) == 0,
          "L2-FIX-B: the exported file is real SVG built from the full (undecimated) block");

    // Sub-threshold guard: a small scatter is passed through verbatim -- no
    // thinning tag, no dropped points, so ordinary classroom graphs are
    // byte-identical to before this fix.
    fs::create_directories(wd + "/small", ec);
    piet::SessionState small(wd + "/small");
    runCmd(small, "set obs 200");
    runCmd(small, "gen x2 = _n");
    runCmd(small, "gen y2 = _n + 1");
    auto jSmall = runCmd(small, "scatter y2 x2");
    auto gSmall = firstBlockOfType(jSmall, "graph");
    check(!gSmall.contains("n_shown_points") && !gSmall.contains("n_total_points"),
          "L2-FIX-B: a sub-budget scatter (200 pts) carries NO decimation tag");
    check(gSmall["points"].is_array() && gSmall["points"].size() == 200,
          "L2-FIX-B: a sub-budget scatter keeps every point (no thinning)");
  }

  // --- BS-012 (D8): `graph export FILE.png` -- honest .svg fallback +
  // explicit "rasterizer pending" warning, NEVER SVG bytes saved under the
  // literal .png name. `as(fmt)` parse (format wins over/supplies a missing
  // extension).
  {
    std::string wd = base + "/export_png_fallback";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    runCmd(session, "sysuse fatalities, clear");
    if (!session.hasData()) {
      // Same isolated/subset-build fixture gap as BS-007/BS-011 above
      // (FIX-SELFTEST-ISOBUILD-CRASH): without fatalities.csv, `scatter`
      // never produces a real graph to export and the unguarded `disp`
      // .value() call below would crash. Skip the whole export-fallback leg.
      skip("BS-012 export-png-fallback leg", "`sysuse fatalities` setup returned no data");
    } else {
      runCmd(session, "scatter fatal beertax");

      // graph-family AI-13: the png is written AS a png at the path given
      // (w2ai gf_exp_004 `file ... saved as PNG format`, log:1939-1951); the
      // .svg sibling fallback and its "rasterizer pending" note are retired.
      std::string pngPath = wd + "/plot.png";
      auto j = runCmd(session, "graph export \"" + pngPath + "\", replace");
      check(firstBlockOfType(j, "error").is_null(), "BS-012 setup: `graph export FILE.png` does not error");
      check(fs::exists(pngPath, ec), "AI-13: the .png is written at the literal path");
      check(readFile(pngPath).rfind("\x89PNG\r\n\x1a\n", 0) == 0,
            "AI-13: the .png file is a real PNG (signature), never SVG bytes under a .png name");
      auto disp = firstBlockOfType(j, "display");
      check(disp.value("text", std::string()).find("saved as PNG format") != std::string::npos,
            "AI-13: the response says the file was saved as PNG (gr_export's own wording)");

      // `as(png)` with an EXTENSIONLESS path: the format comes from as(), and
      // the file is written at the path AS GIVEN -- Stata appends nothing
      // (w2ai gf_exp_asonly_f 0, gf_exp_asonly_png_f 601, log:1987-2001).
      std::string noExtPath = wd + "/noext_plot";
      auto j2 = runCmd(session, "graph export \"" + noExtPath + "\", as(png) replace");
      check(firstBlockOfType(j2, "error").is_null(),
            "BS-012 (D8): `as(png)` on an extensionless filename is honored (format comes from "
            "as(), not the missing extension)");
      check(fs::exists(noExtPath, ec), "AI-13: as(png) writes the bare extensionless path");
      check(!fs::exists(noExtPath + ".png", ec) && !fs::exists(noExtPath + ".svg", ec),
            "AI-13: nothing is written under an appended suffix");

      // `as(svg)` writes real SVG directly to the path AS GIVEN (not the
      // png/pdf fallback branch).
      std::string asSvgPath = wd + "/explicit_as_svg";
      auto j3 = runCmd(session, "graph export \"" + asSvgPath + "\", as(svg) replace");
      check(firstBlockOfType(j3, "error").is_null(), "BS-012: `as(svg)` on an extensionless path succeeds");
      check(fs::exists(asSvgPath, ec),
            "BS-012: `as(svg)` writes to the path AS GIVEN (no .svg appended -- the real format, "
            "not the png/pdf fallback)");

      // Regression guard: an unrecognised extension with no as() is still an
      // honest reject.
      auto j4 = runCmd(session, "graph export \"" + (wd + "/bad.xyz") + "\", replace");
      auto err4 = firstBlockOfType(j4, "error");
      check(!err4.is_null(),
            "BS-012 regression guard: an unrecognised extension (no as()) is still an honest "
            "\"unsupported format\" reject");
    }
  }

  // --- BUG-796..799 (FU-112/FU-113): legend(order(...)) is parsed once into
  // its top-level suboptions (quote- and paren-aware). Oracle: Stata's
  // legend_options -- a quoted key label may hold any character; `off` is a
  // suboption token; order() takes bare indices; an index with no plot is
  // ignored. Every arm below fails on the pre-fix regex parse.
  {
    std::string wd = base + "/legend_order_labels";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    runCmd(session, "clear");
    runCmd(session, "set obs 10");
    runCmd(session, "gen double lx = _n");
    runCmd(session, "gen double ly1 = lx");
    runCmd(session, "gen double ly2 = 2*lx");
    runCmd(session, "gen double ly3 = 3*lx");
    runCmd(session, "gen double ly4 = 4*lx");
    runCmd(session, "label variable ly2 \"Second series\"");
    auto legendOf = [&](const std::string& cmd) {
      auto j = runCmd(session, cmd);
      nlohmann::json g;
      for (const auto& b : j.value("blocks", nlohmann::json::array())) {
        if (b.value("type", std::string()) == "graph") g = b;
      }
      return g.is_object() ? g.value("legend", nlohmann::json::object()) : nlohmann::json::object();
    };
    const std::string four = "twoway (line ly1 lx) (line ly2 lx) (line ly3 lx) (line ly4 lx)";
    const std::string two = "twoway (line ly1 lx) (line ly2 lx)";

    // (1) FU-112: a parenthesised label keeps its entry.
    auto lg1 = legendOf(four + ", legend(order(1 \"a\" 4 \"All ages (pooled)\"))");
    check(lg1.value("order", nlohmann::json::array()) ==
              nlohmann::json::parse("[[0,\"a\"],[3,\"All ages (pooled)\"]]"),
          "BUG-796: legend(order(1 \"a\" 4 \"All ages (pooled)\")) keeps BOTH entries (the label's `)` "
          "ended the old order() capture). Got " + lg1.value("order", nlohmann::json::array()).dump());
    {
      const std::string svg = wd + "/paren.svg";
      runCmd(session, "graph export \"" + svg + "\", replace");
      std::ifstream in(svg);
      std::string txt((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      check(txt.find(">All ages (pooled)</text>") != std::string::npos,
            "BUG-796: ...and the exported SVG carries a <text> reading All ages (pooled)");
    }
    // nested parens in a label (compound quotes are refused upstream of this parser)
    auto lg1b = legendOf(two + ", legend(order(1 \"p (q (r))\" 2 \"z\"))");
    check(lg1b.value("order", nlohmann::json::array()) ==
              nlohmann::json::parse("[[0,\"p (q (r))\"],[1,\"z\"]]"),
          "BUG-796: a label with nested parens parses. Got " +
              lg1b.value("order", nlohmann::json::array()).dump());

    // (2) `off` inside a label is not the off token; legend(off) still is.
    auto lg2 = legendOf(two + ", legend(order(1 \"Payoff\" 2 \"Cutoff\"))");
    check(!lg2.value("off", true) && lg2.value("order", nlohmann::json::array()).size() == 2,
          "BUG-797: legend(order(1 \"Payoff\" 2 \"Cutoff\")) keeps the legend ON with two entries. Got " +
              lg2.dump());
    auto lg2b = legendOf(two + ", legend(off)");
    check(lg2b.value("off", false), "BUG-797 control: legend(off) still turns the legend off");

    // (3) bare indices keep the plot's default key text (y-variable label, else name).
    auto lg3 = legendOf(two + ", legend(order(2 1))");
    check(lg3.value("order", nlohmann::json::array()) ==
              nlohmann::json::parse("[[1,\"Second series\"],[0,\"ly1\"]]"),
          "BUG-798: legend(order(2 1)) reorders with the default key text (variable label, else "
          "name). Got " + lg3.value("order", nlohmann::json::array()).dump());
    auto lg3b = legendOf(two + ", legend(order(2 \"B\" 1))");
    check(lg3b.value("order", nlohmann::json::array()) ==
              nlohmann::json::parse("[[1,\"B\"],[0,\"ly1\"]]"),
          "BUG-798: a bare index beside a labelled one keeps its default text. Got " +
              lg3b.value("order", nlohmann::json::array()).dump());

    // (5) size( / cols( inside a label do not bind to the suboptions.
    auto lg5 = legendOf(two + ", legend(order(1 \"has size(big) here\" 2 \"cols(3) too\"))");
    check(lg5.value("size", nlohmann::json()).is_null() && lg5.value("cols", nlohmann::json()).is_null(),
          "BUG-799: labels containing size( / cols( leave legend.size and legend.cols null. Got " +
              lg5.dump());
    auto lg5b = legendOf(two + ", legend(order(1 \"a\") cols(2) size(small))");
    check(lg5b.value("cols", 0) == 2 && lg5b.value("size", std::string()) == "small",
          "BUG-799 control: real cols(2) size(small) suboptions still bind. Got " + lg5b.dump());

    // (4) FU-113: an out-of-range index leaves no blank row (consecutive y values).
    legendOf(two + ", legend(order(1 \"a\" 3 \"b\" 2 \"c\"))");
    {
      const std::string svg = wd + "/range.svg";
      runCmd(session, "graph export \"" + svg + "\", replace");
      std::ifstream in(svg);
      std::vector<double> ys;
      std::string line;
      while (std::getline(in, line)) {
        if (line.find("text-anchor=\"start\"") == std::string::npos) continue;
        auto p = line.find(" y=\"");
        if (p == std::string::npos) continue;
        ys.push_back(std::strtod(line.c_str() + p + 4, nullptr));
      }
      check(ys.size() == 2 && ys[1] - ys[0] == 14.0,
            "BUG-798/FU-113: order(1 \"a\" 3 \"b\" 2 \"c\") on a 2-plot graph draws exactly 2 legend rows "
            "with no gap. Got " + std::to_string(ys.size()) + " rows" +
                (ys.size() == 2 ? ", dy " + std::to_string(ys[1] - ys[0]) : std::string()));
    }

    // cdfplot shares the off-token rule.
    auto lg6 = legendOf("cdfplot ly1, by(lx) legend(order(1 \"Payoff\"))");
    check(!lg6.value("off", true), "BUG-797: cdfplot legend(order(1 \"Payoff\")) does not switch the legend off. Got " + lg6.dump());
    auto lg6b = legendOf("cdfplot ly1, legend(off)");
    check(lg6b.value("off", false), "BUG-797 control: cdfplot legend(off) still switches it off");
  }

  fs::remove_all(base, ec);
}


void testC1BS027TabulateTwowaySummarize() {
  std::printf("[selftest] --- C1/BS-027 tabulate v1 v2, summarize(v3) two-way form ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_c1_bs027").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);
  (void)piet::dispatch("gen high = (beertax > .5)", session);
  auto blocks = piet::dispatch("tabulate year high, summarize(income)", session);

  const nlohmann::ordered_json* tab = nullptr;
  for (const auto& b : blocks) {
    if (b.is_object() && b.value("type", std::string()) == "tabulate") tab = &b;
  }
  check(tab != nullptr && tab->value("mode", std::string()) == "summarize_twoway",
        "C1/BS-027 setup: `tabulate year high, summarize(income)` returns a two-way summarize "
        "block (previously silently dropped `high` and returned a one-way table)");

  // Row/col labels come from tabulate.cpp's own pyFloatStr-based labelFor()
  // (Python str(float) convention) -- found by PREFIX rather than hardcoded
  // exactly, so this leg doesn't depend on that unrelated formatting detail.
  std::string rowLabel, col0Label, col1Label;
  if (tab != nullptr) {
    if (tab->contains("rows") && (*tab)["rows"].is_array()) {
      for (const auto& r : (*tab)["rows"]) {
        std::string s = r.get<std::string>();
        if (s.rfind("1982", 0) == 0) rowLabel = s;
      }
    }
    if (tab->contains("cols") && (*tab)["cols"].is_array()) {
      for (const auto& c : (*tab)["cols"]) {
        std::string s = c.get<std::string>();
        if (s.rfind("0", 0) == 0) col0Label = s;
        if (s.rfind("1", 0) == 0) col1Label = s;
      }
    }
  }
  check(!rowLabel.empty() && !col0Label.empty() && !col1Label.empty(),
        "C1/BS-027 setup: the two-way table has a 1982 row and 0/1 (high) columns");

  if (tab != nullptr && !rowLabel.empty() && tab->contains("cells") &&
      (*tab)["cells"].contains(rowLabel)) {
    const auto& row1982 = (*tab)["cells"][rowLabel];
    // Oracle (real Stata) + independently re-derived from the raw CSV
    // (sample stdev, n-1 denominator): 1982/high=0: mean=13502.351, N=34;
    // 1982/high=1: mean=11774.037, N=14.
    if (!col0Label.empty() && row1982.contains(col0Label)) {
      const auto& c0 = row1982[col0Label];
      check(c0.contains("mean") && std::abs(c0["mean"].get<double>() - 13502.351) < 0.5,
            "C1/BS-027: cells[1982][high=0].mean ~= 13502.351 (oracle)");
      check(c0.contains("N") && c0["N"].get<int64_t>() == 34,
            "C1/BS-027: cells[1982][high=0].N == 34 (oracle)");
    }
    if (!col1Label.empty() && row1982.contains(col1Label)) {
      const auto& c1 = row1982[col1Label];
      check(c1.contains("mean") && std::abs(c1["mean"].get<double>() - 11774.037) < 0.5,
            "C1/BS-027: cells[1982][high=1].mean ~= 11774.037 (oracle)");
      check(c1.contains("N") && c1["N"].get<int64_t>() == 14,
            "C1/BS-027: cells[1982][high=1].N == 14 (oracle)");
    }
  }
  if (tab != nullptr && tab->contains("grand")) {
    check((*tab)["grand"].contains("mean") &&
              std::abs((*tab)["grand"]["mean"].get<double>() - 13880.185) < 0.5,
          "C1/BS-027: grand.mean ~= 13880.185 (oracle)");
    check((*tab)["grand"].contains("N") && (*tab)["grand"]["N"].get<int64_t>() == 336,
          "C1/BS-027: grand.N == 336 (oracle)");
  }

  // Regression guard: one-way form (single grouping var) is untouched.
  auto blocksOneway = piet::dispatch("tabulate year, summarize(income)", session);
  const nlohmann::ordered_json* tabOneway = nullptr;
  for (const auto& b : blocksOneway) {
    if (b.is_object() && b.value("type", std::string()) == "tabulate") tabOneway = &b;
  }
  check(tabOneway != nullptr && tabOneway->value("mode", std::string()) == "summarize",
        "C1/BS-027 regression guard: one-way `tabulate year, summarize(income)` still uses mode "
        "\"summarize\" (byte-identical old path), not the new two-way mode");

  fs::remove_all(workdir, ec);
}


void testD1BS067TwowayCluster() {
  std::printf("[selftest] --- D1/BS-067 vce(cluster A B) -> --vcov twoway ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_d1_bs067").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);
  piet::SessionState session(workdir);

  (void)piet::dispatch("sysuse fatalities, clear", session);

  // D1i — `reghdfe income beertax, absorb(state) vce(cluster state
  // year)`. Oracle: beertax b=-4181.244, se=1867.524, 48 clusters in
  // state x 7 clusters in year. Tight oracle pin (this shape matches
  // Stata byte-for-byte, live-verified — no engine convention gap, unlike
  // the IV classical/hc1/cluster case above).
  {
    auto blocks = piet::dispatch(
        "reghdfe income beertax, absorb(state) vce(cluster state year)", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr, "D1i: `reghdfe ..., vce(cluster state year)` returns an estimation "
                          "block");
    if (est != nullptr) {
      auto b = coefFieldD1(*est, "beertax", "b");
      auto se = coefFieldD1(*est, "beertax", "se");
      check(b.has_value() && std::abs(*b - (-4181.244)) < 0.5,
            "D1i oracle: beertax b ~= -4181.244, got " +
                (b.has_value() ? std::to_string(*b) : "MISSING"));
      check(se.has_value() && std::abs(*se - 1867.524) < 0.5,
            "D1i oracle: beertax se ~= 1867.524 (tight 2-way cluster match), got " +
                (se.has_value() ? std::to_string(*se) : "MISSING"));
      check(est->value("vcov_type", std::string()).find("twoway") == 0,
            "D1i: vcov_type reads \"twoway(...)\", got \"" +
                est->value("vcov_type", std::string()) + "\"");
    }
  }

  // Legacy `cluster(A B)` synonym (no vce()) hits the SAME twoway path.
  {
    auto blocks =
        piet::dispatch("reghdfe income beertax, absorb(state) cluster(state year)", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr && est->value("vcov_type", std::string()).find("twoway") == 0,
          "D1/BS-067: legacy `cluster(state year)` (no vce()) ALSO forwards --vcov twoway");
  }

  // Three-plus cluster variables: reghdfe forwards k-way multiway clustering
  // (BUG-604). regress's LEGACY cluster() takes ONE clustvar (oracle
  // w2l_vce_shape.log:268-275 regress_opt_c2 -> r(198)); regress's vce(cluster
  // a b c) is Stata multiway at rc 0 (class regress_vce_c2, :227-252) and PIET
  // refuses it by name pending FU-C3-B, so it is never pinned to 198.
  {
    (void)piet::dispatch(
        "capture reghdfe income beertax, absorb(state) vce(cluster state year beertax)", session);
    check(session.eval.locals.get("_rc", "") == "0",
          "D1/BS-067: reghdfe `vce(cluster A B C)` (three vars) runs (k-way multiway)");
    auto blocks = piet::dispatch(
        "reghdfe income beertax, absorb(state) vce(cluster state year beertax)", session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr && est->value("vcov_type", std::string()).find("multiway(") == 0,
          "D1/BS-067: three-var reghdfe cluster vcov_type reads \"multiway(...)\", got \"" +
              (est != nullptr ? est->value("vcov_type", std::string()) : std::string("MISSING")) +
              "\"");
    // e() triple: reghdfe posts `cluster|Robust|<names>` (w2l_vce_shape.log:3822/:3871,
    // w2b_estim.log:1385-1391; matches reghdfe's behaviour, identified from its
    // Mata source). Main refused the fit; 952acb2d
    // published `multiway||`.
    auto tri = piet::dispatch("display \"`e(vce)'|`e(vcetype)'|`e(clustvar)'\"", session);
    const auto* triBlk = firstBlockOfTypeD1(tri, "display");
    const std::string triText = triBlk != nullptr ? triBlk->value("text", std::string()) : "";
    check(triText == "cluster|Robust|state year beertax",
          "D1/BS-067: three-var reghdfe e(vce)|e(vcetype)|e(clustvar) is "
          "`cluster|Robust|state year beertax`, got `" + triText + "`");
  }
  (void)piet::dispatch("capture regress income beertax, cluster(state year beertax)", session);
  check(session.eval.locals.get("_rc", "") == "198",
        "D1/BS-067: legacy regress `cluster(A B C)` refuses r(198), class regress_opt_c2 "
        "(w2l_vce_shape.log:268-275)");
  (void)piet::dispatch("capture regress income beertax, vce(cluster state year beertax)",
                       session);
  {
    // FU-C3-B closed: Stata 19 regress runs vce(cluster a b c) as the built-in
    // multiway VCE at rc 0 (w2l_vce_shape.log:227-252; w2s_multiway_class.log
    // :1115 mw_reg_c3_names).
    const std::string rc = session.eval.locals.get("_rc", "");
    check(rc == "0",
          "D1/BS-067: regress vce(cluster a b c) runs as multiway at rc 0 "
          "(w2l_vce_shape.log:227-252), got " + rc);
  }

  // twoway also reaches through the NEW ivregress arm (BS-063's --vcov
  // reuse of the same splitClusterVarsM4 helper).
  {
    auto blocks = piet::dispatch(
        "ivregress 2sls income emppop (beertax = spirits unemp), vce(cluster state year)",
        session);
    const auto* est = firstBlockOfTypeD1(blocks, "estimation");
    check(est != nullptr && est->value("vcov_type", std::string()).find("twoway") == 0,
          "D1/BS-067: `ivregress 2sls ..., vce(cluster A B)` ALSO forwards --vcov twoway "
          "(shared vcovFromOpts helper)");
  }

  fs::remove_all(workdir, ec);
}


// BETA campaign Phase E (graphics batch, native geometry/options side):
// BUG-099 real lfit/qfit fits, BS-049 kdensity kernel()/Epanechnikov
// default + bandwidth/grid fixes, BS-050 multi-y `scatter y1 y2 x`,
// BS-052 by() honest reject, BS-053 `||` layer separator, BS-054 twoway
// kdensity/histogram umbrella-prefixed forms, BS-055 xtline/tsline SVG
// export real data, BS-056 `graph drop` multi-name + `_all`, BUG-103
// TMPDIR-dependent as(png) extensionless-fallback path bug. All numeric
// legs pinned against a live-Stata oracle probe on the SAME bundled
// fatalities.csv (internal capture
// e-oracle-stata.log) -- not re-derived here.
void testBetaEGraphicsBatch() {
  std::printf("[selftest] --- BETA E graphics batch: BUG-099 lfit/qfit, BS-049/050/052/053/054/055/056, "
              "BUG-103 ---\n");
  namespace fs = std::filesystem;
  std::string base = (fs::temp_directory_path() / "piet_native_selftest_betae_graphics").u8string();
  std::error_code ec;
  fs::remove_all(base, ec);
  fs::create_directories(base, ec);

  auto runCmd = [](piet::SessionState& session, const std::string& cmd) {
    nlohmann::json body{{"cmd", cmd}};
    auto r = piet::routes::command(session, body.dump());
    return nlohmann::json::parse(r.json);
  };
  auto firstBlockOfType = [](const nlohmann::json& j, const std::string& type) -> nlohmann::json {
    for (const auto& b : j.value("blocks", nlohmann::json::array())) {
      if (b.value("type", std::string()) == type) return b;
    }
    return nlohmann::json();
  };
  auto findLayerByKind = [](const nlohmann::json& twowayBlock, const std::string& kind) -> nlohmann::json {
    for (const auto& l : twowayBlock.value("layers", nlohmann::json::array())) {
      if (l.value("kind", std::string()) == kind) return l;
    }
    return nlohmann::json();
  };
  auto readFile = [](const std::string& path) -> std::string {
    std::ifstream in(cecon::to_native_path(path), std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
  };

  // Every leg below (BUG-099, BS-049/050/052/053/054/055/056, BUG-103) opens
  // with `sysuse fatalities, clear` and then chains
  // firstBlockOfType(...).value(...)/findLayerByKind(...) unguarded. In the
  // isolated/subset-build environment (no piet/packaging/sample/
  // fatalities.csv), that throws the same json.exception.type_error.306 as
  // the earlier BUG-039/BUG-055/BUG-056/BS-013/BS-048/BS-047/BS-008-057 legs
  // (FIX-SELFTEST-ISOBUILD-CRASH). One up-front probe covers every leg below.
  {
    std::string probeWd = base + "/_fixture_probe";
    fs::create_directories(probeWd, ec);
    piet::SessionState probeSession(probeWd);
    runCmd(probeSession, "sysuse fatalities, clear");
    if (!probeSession.hasData()) {
      skip("BETA E graphics batch (all legs: BUG-099, BS-049/050/052/053/054/055/056, BUG-103)",
           "`sysuse fatalities` setup returned no data");
      fs::remove_all(base, ec);
      return;
    }
  }

  // --- BUG-099: `lfit`/`qfit` real OLS fits (parenthesised-layer form).
  // Oracle: `regress income beertax` on fatalities.csv (all 336 obs, no
  // if-filter) -> b_cons=14842.18, b_beertax=-1874.3005; xmin=.04331088,
  // xmax=2.7207637; yhat_at_xmin=14761.003, yhat_at_xmax=9742.6517.
  // `regress income beertax beertax2` -> b_cons=15931.426,
  // b_beertax=-5823.7769, b_beertax2=1909.7359.
  {
    std::string wd = base + "/bug099_lfit_qfit";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    runCmd(session, "sysuse fatalities, clear");

    auto jLfit = runCmd(session, "twoway (scatter income beertax) (lfit income beertax)");
    check(firstBlockOfType(jLfit, "error").is_null(), "BUG-099 setup: scatter+lfit twoway succeeds");
    auto graphLfit = firstBlockOfType(jLfit, "graph");
    auto lfitLayer = findLayerByKind(graphLfit, "lfit");
    check(!lfitLayer.is_null(), "BUG-099: the twoway result has a layer with kind==\"lfit\" (not "
                                 "aliased to \"line\")");
    if (!lfitLayer.is_null()) {
      auto pts = lfitLayer.value("points", nlohmann::json::array());
      check(pts.size() == 2,
            "BUG-099: lfit renders as a 2-point line (a real fit), not a 336-point connect-the-dots "
            "polyline through the raw data — got " + std::to_string(pts.size()) + " points");
      if (pts.size() == 2) {
        check(std::abs(pts[0][0].get<double>() - 0.04331088) < 1e-4,
              "BUG-099: lfit's first point x == the data's own xmin (r(min))");
        check(std::abs(pts[0][1].get<double>() - 14761.003) < 0.5,
              "BUG-099: lfit's fitted y at xmin matches the oracle's yhat_at_xmin=14761.003");
        check(std::abs(pts[1][0].get<double>() - 2.7207637) < 1e-4,
              "BUG-099: lfit's last point x == the data's own xmax (r(max))");
        check(std::abs(pts[1][1].get<double>() - 9742.6517) < 0.5,
              "BUG-099: lfit's fitted y at xmax matches the oracle's yhat_at_xmax=9742.6517");
      }
      double cons = lfitLayer.value("coef", nlohmann::json::object()).value("cons", 0.0);
      double slope = lfitLayer.value("coef", nlohmann::json::object()).value("slope", 0.0);
      check(std::abs(cons - 14842.18) < 0.05, "BUG-099: lfit b_cons matches oracle 14842.18");
      check(std::abs(slope - (-1874.3005)) < 0.01, "BUG-099: lfit b_beertax matches oracle -1874.3005");
    }

    auto jQfit = runCmd(session, "twoway (scatter income beertax) (qfit income beertax)");
    check(firstBlockOfType(jQfit, "error").is_null(), "BUG-099 setup: scatter+qfit twoway succeeds");
    auto graphQfit = firstBlockOfType(jQfit, "graph");
    auto qfitLayer = findLayerByKind(graphQfit, "qfit");
    check(!qfitLayer.is_null(), "BUG-099: the twoway result has a layer with kind==\"qfit\"");
    if (!qfitLayer.is_null()) {
      auto pts = qfitLayer.value("points", nlohmann::json::array());
      check(pts.size() == 100, "BUG-099: qfit samples the fitted curve at n=100 points, got " +
                                    std::to_string(pts.size()));
      double cons = qfitLayer.value("coef", nlohmann::json::object()).value("cons", 0.0);
      double b1 = qfitLayer.value("coef", nlohmann::json::object()).value("b1", 0.0);
      double b2 = qfitLayer.value("coef", nlohmann::json::object()).value("b2", 0.0);
      check(std::abs(cons - 15931.426) < 0.5, "BUG-099: qfit b_cons matches oracle 15931.426");
      check(std::abs(b1 - (-5823.7769)) < 0.5, "BUG-099: qfit b_beertax matches oracle -5823.7769");
      check(std::abs(b2 - 1909.7359) < 0.5, "BUG-099: qfit b_beertax2 matches oracle 1909.7359");
    }
  }

  // --- BS-049: kdensity kernel()/Epanechnikov default, bandwidth 1.349
  // constant, and the xmin-h/xmax+h grid range (was xmin-3h/xmax+3h).
  // Oracle: `kdensity income, n(50)` -> bwidth=633.4973; kx/kd row 1 =
  // (8880.2644, 4.979e-06); row 5 = (10018.77, .00003834); row 50 =
  // (22826.952, 1.261e-06). Gaussian variant row 1 = (8880.2644, 4.915e-06).
  {
    std::string wd = base + "/bs049_kdensity_kernel";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    runCmd(session, "sysuse fatalities, clear");

    auto jEpan = runCmd(session, "kdensity income, n(50)");
    check(firstBlockOfType(jEpan, "error").is_null(), "BS-049 setup: default kdensity succeeds");
    auto gEpan = firstBlockOfType(jEpan, "graph");
    check(gEpan.value("kernel", std::string()) == "epanechnikov",
          "BS-049: default kernel is epanechnikov (Stata's own default), not gaussian");
    check(std::abs(gEpan.value("bwidth", 0.0) - 633.4973) < 0.01,
          "BS-049: bandwidth matches the oracle's Silverman-rule value 633.4973 (1.349 constant)");
    auto ptsEpan = gEpan.value("points", nlohmann::json::array());
    check(ptsEpan.size() == 50, "BS-049 setup: n(50) grid has 50 points");
    if (ptsEpan.size() == 50) {
      check(std::abs(ptsEpan[0][0].get<double>() - 8880.2644) < 0.01,
            "BS-049: grid point 1's x == oracle's kx[1]=8880.2644 (xmin-1*bwidth, was xmin-3*bwidth)");
      check(std::abs(ptsEpan[0][1].get<double>() - 4.979e-06) < 5e-8,
            "BS-049: grid point 1's density matches oracle Epanechnikov kd[1]=4.979e-06");
      check(std::abs(ptsEpan[4][1].get<double>() - 3.834e-05) < 5e-7,
            "BS-049: grid point 5's density matches oracle Epanechnikov kd[5]=.00003834");
      check(std::abs(ptsEpan[49][0].get<double>() - 22826.952) < 0.01,
            "BS-049: grid point 50's x == oracle's kx[50]=22826.952 (xmax+1*bwidth)");
      check(std::abs(ptsEpan[49][1].get<double>() - 1.261e-06) < 5e-8,
            "BS-049: grid point 50's density matches oracle Epanechnikov kd[50]=1.261e-06");
    }

    auto jGauss = runCmd(session, "kdensity income, n(50) kernel(gaussian)");
    check(firstBlockOfType(jGauss, "error").is_null(), "BS-049 setup: kernel(gaussian) succeeds");
    auto gGauss = firstBlockOfType(jGauss, "graph");
    check(gGauss.value("kernel", std::string()) == "gaussian", "BS-049: kernel(gaussian) is honored");
    auto ptsGauss = gGauss.value("points", nlohmann::json::array());
    if (ptsGauss.size() == 50) {
      check(std::abs(ptsGauss[0][1].get<double>() - 4.915e-06) < 5e-8,
            "BS-049: kernel(gaussian) grid point 1's density matches oracle kd[1]=4.915e-06 "
            "(different value from the Epanechnikov default -- proves kernel() is actually read)");
    }

    // gfx-histogram-kdensity-family: all eight kernels are served now, so an
    // unknown name is Stata's own refusal (w2aq_histogram_family hk_r_kd_kern:
    // `invalid kernel function`, r(198)), not a "supported here" gap.
    auto jBadKernel = runCmd(session, "kdensity income, kernel(uniform)");
    auto errBadKernel = firstBlockOfType(jBadKernel, "error");
    check(!errBadKernel.is_null() && errBadKernel.value("message", std::string()) == "invalid kernel function" &&
              errBadKernel.value("reason", std::string()) == "r(198)",
          "BS-049: an unknown kernel() name is refused as Stata refuses it (r(198) invalid kernel "
          "function), not a silent substitution");
  }

  // --- BS-050: `scatter y1 y2 x` multi-y -- last var is x, every var
  // before it is an independent y-series.
  {
    std::string wd = base + "/bs050_multiy_scatter";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    runCmd(session, "sysuse fatalities, clear");

    auto jSingle = runCmd(session, "scatter income beertax");
    check(firstBlockOfType(jSingle, "error").is_null(), "BS-050 regression: single-y scatter still works");
    auto gSingle = firstBlockOfType(jSingle, "graph");
    check(!gSingle.contains("series"),
          "BS-050 regression: the single-y (2-token) case keeps the EXACT legacy flat JSON shape -- "
          "no \"series\" key at all");
    check(gSingle.value("x_var", std::string()) == "beertax" && gSingle.value("y_var", std::string()) == "income",
          "BS-050 regression: single-y x_var/y_var unchanged");

    auto jMulti = runCmd(session, "scatter income spirits beertax");
    check(firstBlockOfType(jMulti, "error").is_null(), "BS-050: `scatter income spirits beertax` succeeds");
    auto gMulti = firstBlockOfType(jMulti, "graph");
    check(gMulti.value("x_var", std::string()) == "beertax",
          "BS-050: the LAST token (beertax) is the shared x, not the first");
    auto series = gMulti.value("series", nlohmann::json::array());
    check(series.size() == 2, "BS-050: two y-series (income, spirits) registered, got " +
                                   std::to_string(series.size()));
    if (series.size() == 2) {
      check(series[0].value("y_var", std::string()) == "income" &&
                series[1].value("y_var", std::string()) == "spirits",
            "BS-050: series are in the typed order income, spirits (not alphabetized/reordered)");
      check(series[0]["n"].get<int64_t>() > 0 && series[1]["n"].get<int64_t>() > 0,
            "BS-050: both series carry real (non-empty) point data");
    }
  }

  // --- BS-052: `by()` real faceting is honestly rejected everywhere it's
  // read (scatter/histogram/kdensity/twoway), not silently dropped.
  {
    std::string wd = base + "/bs052_by_facet_reject";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    runCmd(session, "sysuse fatalities, clear");

    auto jScatterBy = runCmd(session, "scatter income beertax, by(state)");
    check(firstBlockOfType(jScatterBy, "error").value("code", std::string()) == "unsupported_option",
          "BS-052: `scatter ..., by(state)` is an honest unsupported_option reject");
    // gfx-histogram-kdensity-family: histogram's by() is served (one panel per
    // group, common start and width: w2aq_histogram_family hk_h_bystr, hk_h_byfw).
    auto jHistBy = runCmd(session, "histogram income, by(state)");
    auto gHistBy = firstBlockOfType(jHistBy, "graph");
    check(firstBlockOfType(jHistBy, "error").is_null() && !gHistBy.is_null() &&
              gHistBy.value("panels", nlohmann::json::array()).size() > 1,
          "BS-052: `histogram ..., by(state)` draws one panel per state");
    auto jKdensBy = runCmd(session, "kdensity income, by(state)");
    check(firstBlockOfType(jKdensBy, "error").value("code", std::string()) == "unsupported_option",
          "BS-052: `kdensity ..., by(state)` is an honest unsupported_option reject");
    auto jTwowayBy = runCmd(session, "twoway (scatter income beertax), by(state)");
    check(firstBlockOfType(jTwowayBy, "error").value("code", std::string()) == "unsupported_option",
          "BS-052: parenthesised `twoway (scatter ...), by(state)` (global option) is an honest "
          "unsupported_option reject");
  }

  // --- BS-053: `||` double-pipe layer separator -- an alternate spelling
  // of the parenthesised multi-layer form, same layer semantics.
  {
    std::string wd = base + "/bs053_double_pipe";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    runCmd(session, "sysuse fatalities, clear");

    auto jPipe = runCmd(session, "twoway scatter income beertax || lfit income beertax");
    check(firstBlockOfType(jPipe, "error").is_null(), "BS-053: `scatter ... || lfit ...` succeeds");
    auto gPipe = firstBlockOfType(jPipe, "graph");
    check(gPipe.value("kind", std::string()) == "twoway",
          "BS-053: `||` routes through the SAME multi-layer twoway path as the parenthesised form");
    auto layers = gPipe.value("layers", nlohmann::json::array());
    check(layers.size() == 2,
          "BS-053: `||` produced TWO layers (was silently truncating to just the first plot) -- got " +
              std::to_string(layers.size()));
    if (layers.size() == 2) {
      check(layers[0].value("kind", std::string()) == "scatter" &&
                layers[1].value("kind", std::string()) == "lfit",
            "BS-053: layer order/kinds preserved (scatter, then lfit)");
      auto pts = layers[1].value("points", nlohmann::json::array());
      check(pts.size() == 2, "BS-053: the `||`-form lfit layer is a real 2-point fit, not raw data");
    }

    // A single `|` (Stata's logical-or inside an `if`) must NOT be
    // mistaken for the `||` layer separator.
    auto jSingleBar = runCmd(session, "twoway scatter income beertax if year==1982 | year==1983");
    check(firstBlockOfType(jSingleBar, "error").is_null(),
          "BS-053 regression: a single `|` inside an `if` expression still parses fine (not "
          "mistaken for `||`)");
  }

  // --- BS-054: `twoway kdensity x` / `twoway histogram x` (umbrella-
  // prefixed spellings) route to the SAME already-working runKdensity/
  // runHistogram, instead of "variable kdensity not found".
  {
    std::string wd = base + "/bs054_twoway_prefixed";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    runCmd(session, "sysuse fatalities, clear");

    // serset-twoway HKL: `twoway kdensity|histogram` is the twoway PLOT TYPE
    // (twoway__kdensity_gen's 300-point grid over [min, max]; no start() unless
    // given -- w2al tf_kd_n log:3524-3540, tf_hist_082), drawn as a twoway graph
    // with one kdensity / histogram layer, not the kdensity/histogram COMMAND's
    // block.
    auto jTwKdens = runCmd(session, "twoway kdensity income");
    check(firstBlockOfType(jTwKdens, "error").is_null(),
          "BS-054: `twoway kdensity income` no longer errors \"variable kdensity not found\"");
    auto gTwKdens = firstBlockOfType(jTwKdens, "graph");
    auto kdLayers = gTwKdens.value("layers", nlohmann::json::array());
    check(gTwKdens.value("kind", std::string()) == "twoway" && kdLayers.size() == 1 &&
              kdLayers[0].value("kind", std::string()) == "kdensity",
          "BS-054: `twoway kdensity income` renders a real kdensity layer");

    auto jTwHist = runCmd(session, "twoway histogram income");
    check(firstBlockOfType(jTwHist, "error").is_null(),
          "BS-054: `twoway histogram income` no longer errors \"variable histogram not found\"");
    auto gTwHist = firstBlockOfType(jTwHist, "graph");
    auto hLayers = gTwHist.value("layers", nlohmann::json::array());
    check(gTwHist.value("kind", std::string()) == "twoway" && hLayers.size() == 1 &&
              hLayers[0].value("kind", std::string()) == "histogram",
          "BS-054: `twoway histogram income` renders a real histogram layer");

    // serset-twoway SS: every graph leaves its sersets in the session store
    // (w2al_twoway_family.log:5380-5456): a data plot and a fit are two sersets,
    // the current one the last made; `graph drop _all` takes them with it.
    runCmd(session, "graph drop _all");
    runCmd(session, "serset clear");
    runCmd(session, "twoway (scatter income beertax) (lfit income beertax), name(ssG, replace)");
    const auto* ss0 = session.sersets.get(0);
    const auto* ss1 = session.sersets.get(1);
    check(session.sersets.all().size() == 2 && session.sersets.current() == 1 && ss0 != nullptr &&
              ss1 != nullptr && ss0->cols.size() == 2 && ss0->cols[0].name == "income" &&
              ss1->n == 3 && ss1->cols.size() == 2 && ss1->cols[1].name == "beertax" &&
              ss1->cols[1].type == "float",
          "SS: `twoway (scatter) (lfit)` leaves 2 sersets (income beertax; the 3-point fit grid)");
    runCmd(session, "graph drop _all");
    check(session.sersets.empty() && session.sersets.current() == -1,
          "SS: `graph drop _all` drops every serset");
  }

  // --- BS-055: xtline/tsline `graph export` now draws real data, not just
  // an empty frame (buildSvgDispatch had no kind branch for either, so both
  // fell through to buildSvgScatter's "points" key, which neither block
  // shape carries).
  {
    std::string wd = base + "/bs055_xtline_tsline_export";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    runCmd(session, "sysuse fatalities, clear");
    runCmd(session, "egen sid = group(state)");  // xtset refuses a string panel (BUG-1164)
    runCmd(session, "xtset sid year");  // also seeds session.timevar for tsline

    auto jXt = runCmd(session, "xtline fatal");
    check(firstBlockOfType(jXt, "error").is_null(), "BS-055 setup: `xtline fatal` succeeds");
    std::string xtSvgPath = wd + "/xtline.svg";
    auto jXtExp = runCmd(session, "graph export \"" + xtSvgPath + "\", replace");
    check(firstBlockOfType(jXtExp, "error").is_null(), "BS-055 setup: xtline `graph export` succeeds");
    check(fs::exists(xtSvgPath, ec), "BS-055 setup: the xtline export actually wrote a file");
    std::string xtSvg = readFile(xtSvgPath);
    check(xtSvg.find("<path") != std::string::npos,
          "BS-055: the exported xtline SVG contains real <path> data (was frame/axes/title only, "
          "zero data)");

    auto jTs = runCmd(session, "tsline fatal");
    check(firstBlockOfType(jTs, "error").is_null(), "BS-055 setup: `tsline fatal` succeeds");
    std::string tsSvgPath = wd + "/tsline.svg";
    auto jTsExp = runCmd(session, "graph export \"" + tsSvgPath + "\", replace");
    check(firstBlockOfType(jTsExp, "error").is_null(), "BS-055 setup: tsline `graph export` succeeds");
    std::string tsSvg = readFile(tsSvgPath);
    check(tsSvg.find("<path") != std::string::npos,
          "BS-055: the exported tsline SVG contains real <path> data (was frame/axes/title only, "
          "zero data)");
  }

  // --- BS-056: `graph drop namelist | _all` -- multi-name + honest reject
  // on an unknown name, `_all` clears everything.
  {
    std::string wd = base + "/bs056_graph_drop";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    runCmd(session, "sysuse fatalities, clear");

    runCmd(session, "scatter income beertax, name(g1)");
    runCmd(session, "scatter income spirits, name(g2)");
    check(session.namedGraphs.count("g1") == 1 && session.namedGraphs.count("g2") == 1,
          "BS-056 setup: g1 and g2 both registered");

    auto jDrop2 = runCmd(session, "graph drop g1 g2");
    check(firstBlockOfType(jDrop2, "error").is_null(), "BS-056: `graph drop g1 g2` (multi-name) succeeds");
    check(session.namedGraphs.count("g1") == 0 && session.namedGraphs.count("g2") == 0,
          "BS-056: BOTH g1 and g2 are gone (was first-name-only -- g2 used to survive silently)");

    auto jDropMissing = runCmd(session, "graph drop g1");
    auto errMissing = firstBlockOfType(jDropMissing, "error");
    check(!errMissing.is_null(),
          "BS-056: dropping an already-gone name is an honest reject, not a silent \"(graph "
          "dropped)\" false success");
    check(errMissing.value("message", std::string()).find("g1") != std::string::npos,
          "BS-056: the reject names the missing graph");

    runCmd(session, "scatter income beertax, name(g3)");
    runCmd(session, "scatter income spirits, name(g4)");
    auto jDropAll = runCmd(session, "graph drop _all");
    check(firstBlockOfType(jDropAll, "error").is_null(), "BS-056: `graph drop _all` succeeds");
    check(session.namedGraphs.empty(), "BS-056: `_all` clears every named graph");
  }

  // --- BUG-103: `as(png)` extensionless-path fallback must resolve the
  // same regardless of whether the workdir/TMPDIR happens to contain a
  // dot in a path component (e.g. a scratch dir literally named
  // `.dotted-scratch`, standing in for a real dotted TMPDIR like
  // `<volume>/<workspace>/.tmp-scratch`) -- this reproduces the bug
  // in-process without depending on the ambient TMPDIR env var.
  {
    std::string wd = base + "/bug103_dotted_dir";
    std::string dottedDir = wd + "/.dotted-scratch";
    fs::create_directories(dottedDir, ec);
    piet::SessionState session(wd);
    runCmd(session, "sysuse fatalities, clear");
    runCmd(session, "scatter income beertax");

    std::string noExtPath = dottedDir + "/noext_plot";
    auto j = runCmd(session, "graph export \"" + noExtPath + "\", as(png) replace");
    check(firstBlockOfType(j, "error").is_null(),
          "BUG-103: `as(png)` on an extensionless path under a DOTTED directory still succeeds");
    // graph-family AI-13: the file lands at the path as given (w2ai
    // gf_exp_asonly_f, log:1987-2001), inside the dotted directory -- the
    // directory's dot is never read as the file's extension.
    check(fs::exists(noExtPath, ec),
          "BUG-103: the png lands at the CORRECT path (dottedDir/noext_plot), not a path truncated "
          "at the directory's dot");
    if (fs::exists(noExtPath, ec)) {
      check(readFile(noExtPath).rfind("\x89PNG", 0) == 0, "BUG-103: the file is a real PNG");
    }
  }

  fs::remove_all(base, ec);
}


// --- BUG-129 (twoway unsupported-layer silent-drop) + BUG-130 (bare-form
// rarea band) graph-honesty legs. BUG-129: an unsupported twoway plot type is
// now a reject-BEFORE-output error (the whole command fails, naming the kind),
// not a silently-missing layer / an empty-plot-that-"succeeds". BUG-130: the
// native paren-form rarea band builder already emits its 3-var band
// (layerRarea, unchanged since the analysis.do port); the reproducible native
// gap was the BARE `twoway rarea ...` spelling, which misrouted to runScatter
// and never reached the builder — it now takes the same layer machinery.
// (graph.js's rarea band <path> renderer stays covered by the DOM runner's
// Stage-4 suite — untouched here.)
void testBUG129And130GraphHonesty() {
  std::printf("[selftest] --- BUG-129 twoway unsupported-layer reject + BUG-130 rarea band ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_bug129_130").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  auto runCmd = [](piet::SessionState& session, const std::string& cmd) {
    nlohmann::json body{{"cmd", cmd}};
    auto r = piet::routes::command(session, body.dump());
    return nlohmann::json::parse(r.json);
  };
  auto firstBlockOfType = [](const nlohmann::json& j, const std::string& type) -> nlohmann::json {
    for (const auto& b : j.value("blocks", nlohmann::json::array())) {
      if (b.value("type", std::string()) == type) return b;
    }
    return nlohmann::json();
  };
  // W4: firstBlockOfType returns a NULL json when the block is absent, and
  // nlohmann's `.value()` THROWS on a null -- which aborts the whole selftest
  // binary (exit 134) instead of failing one check. That was measured, not
  // guessed: the first seeded-RED run of the W4 legs below aborted at the first
  // leg whose block the seeded defect made disappear, and every leg after it
  // stopped reporting. A seeded-RED that cannot say WHICH legs it turned has no
  // discriminating power, so these legs read through `blockOrEmpty`.
  auto blockOrEmpty = [&firstBlockOfType](const nlohmann::json& j,
                                          const std::string& type) -> nlohmann::json {
    nlohmann::json b = firstBlockOfType(j, type);
    return b.is_object() ? b : nlohmann::json::object();
  };

  piet::SessionState session(workdir);
  (void)runCmd(session, "clear");
  (void)runCmd(session, "set obs 5");
  (void)runCmd(session, "gen x = _n");
  (void)runCmd(session, "gen lo = _n");
  (void)runCmd(session, "gen hi = _n + 2");
  check(session.hasData() && session.datastore()->has("hi") && session.datastore()->has("lo") &&
            session.datastore()->has("x"),
        "BUG-130 setup: 5-row band fixture (hi/lo/x) built in memory");

  // BUG-130 (paren form): the 3-var rarea builder emits a full band (one
  // vertex per row), NOT an empty series.
  {
    auto j = runCmd(session, "twoway (rarea hi lo x)");
    auto g = firstBlockOfType(j, "graph");
    check(firstBlockOfType(j, "error").is_null(),
          "BUG-130: `twoway (rarea hi lo x)` is not an error");
    auto layers = g.value("layers", nlohmann::json::array());
    check(!layers.empty() && layers[0].value("kind", std::string()) == "rarea",
          "BUG-130: paren-form layer kind is `rarea`");
    check(!layers.empty() && layers[0].value("band", nlohmann::json::array()).size() == 5,
          "BUG-130: rarea band carries one vertex per data row (5), not 0 points");
  }

  // BUG-130 (bare form): `twoway rarea hi lo x` (no parens) now reaches the
  // SAME band builder instead of misrouting to a 2-var scatter.
  {
    auto j = runCmd(session, "twoway rarea hi lo x");
    auto g = firstBlockOfType(j, "graph");
    auto layers = g.value("layers", nlohmann::json::array());
    check(!layers.empty() && layers[0].value("kind", std::string()) == "rarea" &&
              layers[0].value("band", nlohmann::json::array()).size() == 5,
          "BUG-130: bare `twoway rarea hi lo x` builds a 5-vertex band (no silent scatter misroute)");
  }

  // BUG-348: the `%NN` opacity suffix. Coverage here was ZERO before this leg —
  // no selftest ever passed a `%NN` to any color option — which is how a corpus
  // that asks for 15%-opaque CI bands shipped 20%-opaque ones through eight waves,
  // and how the underlying UB (parseColor read its second capture group AFTER
  // reassigning the string those iterators pointed into) went unnoticed until a
  // parallel run rendered the SAME band at 0.07 in one arm and 0.2 in another.
  //
  // Both layer families are asserted: rarea reads the suffix off `fcolor`, the
  // line family off `lcolor`, and they reach opacity through different branches of
  // layerStyle. The no-suffix case is pinned too — a fix that made every color
  // opaque-by-default would satisfy the first two legs alone.
  {
    auto j = runCmd(session, "twoway (rarea hi lo x, fcolor(cranberry%15) lwidth(none))");
    auto layers = firstBlockOfType(j, "graph").value("layers", nlohmann::json::array());
    const double op = layers.empty() ? -1.0
                                     : layers[0].value("style", nlohmann::json::object())
                                           .value("opacity", -1.0);
    check(std::fabs(op - 0.15) < 1e-12,
          "BUG-348: `fcolor(cranberry%15)` renders at 0.15, not the no-opacity fallback 0.2 "
          "(got " + std::to_string(op) + ")");
  }
  {
    auto j = runCmd(session, "twoway (connected hi x, lcolor(navy%30))");
    auto layers = firstBlockOfType(j, "graph").value("layers", nlohmann::json::array());
    const double op = layers.empty() ? -1.0
                                     : layers[0].value("style", nlohmann::json::object())
                                           .value("opacity", -1.0);
    check(std::fabs(op - 0.30) < 1e-12,
          "BUG-348: `lcolor(navy%30)` on a line layer renders at 0.30 (got " +
              std::to_string(op) + ")");
  }
  {
    auto j = runCmd(session, "twoway (connected hi x, lcolor(navy))");
    auto layers = firstBlockOfType(j, "graph").value("layers", nlohmann::json::array());
    const double op = layers.empty() ? -1.0
                                     : layers[0].value("style", nlohmann::json::object())
                                           .value("opacity", -1.0);
    check(std::fabs(op - 1.0) < 1e-12,
          "BUG-348: a color with NO %NN stays fully opaque (got " + std::to_string(op) + ")");
  }

  // BUG-129: an unsupported layer plot type fails the WHOLE command with an
  // honest error naming the kind — no partial plot (reject-before-output).
  // serset-twoway FIT: lfitci is served now (w2al tf_lfitci_* sersets,
  // log:2068-2143), so the leg names `fpfit`, a Stata plot type PIET still lacks.
  {
    auto j = runCmd(session, "twoway (scatter hi x) (fpfit hi x)");
    auto err = firstBlockOfType(j, "error");
    check(err.value("code", std::string()) == "unsupported_command",
          "BUG-129: an unsupported twoway layer (`fpfit`) is an honest unsupported_command");
    check(err.value("message", std::string()).find("fpfit") != std::string::npos,
          "BUG-129: the reject message names the offending plot type (`fpfit`)");
    check(firstBlockOfType(j, "graph").is_null(),
          "BUG-129: NO graph block is emitted alongside the reject (no partial plot)");
  }

  // BUG-129: a lone unsupported layer no longer renders an empty plot that
  // "succeeds" — it is a reject.
  {
    // serset-twoway RNG: rcap/rspike are drawn now (w2al tf_rcap_*), so the
    // lone-unsupported-layer legs name pcspike, a Stata type PIET still lacks.
    auto j = runCmd(session, "twoway (pcspike hi x lo x)");
    check(!firstBlockOfType(j, "error").is_null() && firstBlockOfType(j, "graph").is_null(),
          "BUG-129: a lone `(pcspike ...)` is a reject, not an empty successful plot");
  }

  // BUG-129: the reject carries a nonzero Stata rc through `capture`
  // (unsupported_command -> r(199)).
  {
    piet::dispatch("capture twoway (pcspike hi x lo x)", session);
    check(session.eval.locals.get("_rc", "") == "199",
          "BUG-129: an unsupported twoway layer inside `capture` sets _rc=199");
  }

  // Over-rejection guard: a twoway of only SUPPORTED layer types still renders
  // cleanly (scatter + lfit), no error.
  {
    auto j = runCmd(session, "twoway (scatter hi x) (lfit hi x)");
    check(firstBlockOfType(j, "error").is_null() && !firstBlockOfType(j, "graph").is_null(),
          "BUG-129 guard: `twoway (scatter ...) (lfit ...)` (all supported) still renders");
  }

  // ==========================================================================
  // AER scope-2 W4 -- the graph option vocabulary.
  //
  // Two separate defects, both measured on main the day this was written:
  //   (1) `twoway (scatter y x) (line y x), xlabel(1 2) bogusopt(1)` -> rc 0.
  //       The multi-layer path had NO unknown-option net on either plane, so a
  //       typed option name was accepted, dropped and never mentioned. THE
  //       SEEDED-RED for this leg is the net itself: delete the net block in
  //       graph_m4.cpp's runTwowayLayers and the two `bogus` legs below go RED
  //       while everything else stays GREEN.
  //   (2) `scatter y x, xline(0)` -> r(198), while the multi-layer renderer
  //       READ xline all along. A wiring gap: one surface could not reach
  //       machinery the other used.
  // ==========================================================================

  // (2) The conditional re-route: a layer-honoured global moves the statement
  // into the layer renderer AND THE VALUE LANDS. Asserting only kind=="twoway"
  // would pass for a re-route that then ignored the option.
  {
    auto j = runCmd(session, "scatter hi x, xline(0) yscale(range(0 10))");
    auto g = blockOrEmpty(j, "graph");
    check(blockOrEmpty(j, "error").empty(),
          "W4: `scatter y x, xline(0)` is no longer r(198) — the layer renderer reads xline");
    check(g.value("kind", std::string()) == "twoway",
          "W4: it re-routes to the layer renderer (kind==\"twoway\"), which is the only one "
          "that draws axes");
    auto xl = g.value("axes", nlohmann::json::object()).value("xline", nlohmann::json::array());
    check(xl.size() == 1 && std::fabs(xl[0].get<double>() - 0.0) < 1e-12,
          "W4: and the reference line VALUE lands in the block (axes.xline == [0])");
  }

  // (2, the other direction) THE PIN CONSTRAINT. A bare command must not move:
  // six freeze fixtures pin these blocks byte for byte and two older legs assert
  // kind=="scatter". A title-only command must not move either.
  {
    auto g = blockOrEmpty(runCmd(session, "scatter hi x"), "graph");
    check(g.value("kind", std::string()) == "scatter" && g.contains("points") &&
              g.contains("x_var"),
          "W4 pin: a BARE `scatter y x` keeps the flat kind==\"scatter\" shape (freeze "
          "fixtures scatter/graph/line/tsline/histogram/kdensity pin it byte for byte)");
    auto g2 = blockOrEmpty(runCmd(session, "scatter hi x, title(\"t\")"), "graph");
    check(g2.value("kind", std::string()) == "scatter" && g2.value("title", std::string()) == "t",
          "W4 pin: a TITLE-ONLY `scatter` does not move either — only the eight layer-honoured "
          "globals re-route");
  }

  // (2, the guard the varlist forced) `scatter y1 y2 x` is Stata's shared-x
  // multi-series form and LayerSpec carries one y/x pair, so a 3-token varlist
  // must stay with runScatter even when it asks for an axis option.
  {
    auto g = blockOrEmpty(runCmd(session, "scatter hi lo x, xline(0)"), "graph");
    check(g.value("kind", std::string()) == "scatter" &&
              g.value("series", nlohmann::json::array()).size() == 2,
          "W4 guard: a 3-variable `scatter y1 y2 x` keeps BOTH series (re-routing it would "
          "silently drop everything after the first)");
  }

  // (3) A cosmetic option is accepted and the note NAMES it. Silence here would
  // be the same defect class as (1) wearing a success's clothes.
  {
    auto j = runCmd(session, "histogram hi, graphregion(color(white))");
    auto g = blockOrEmpty(j, "graph");
    check(blockOrEmpty(j, "error").empty(),
          "W4: `histogram y, graphregion(...)` is rc 0 (was r(198) `not yet implemented`)");
    const std::string note = g.value("note", std::string());
    check(note.find("graphregion") != std::string::npos &&
              note.find("accepted and not applied") != std::string::npos,
          "W4: and the block's note NAMES it — accepted, never silently dropped (got \"" +
              note + "\")");
  }
  // ... and an option that moves a NUMBER is honoured, not noted:
  // gfx-histogram-kdensity-family serves `discrete` (one bin per distinct value,
  // w2aq_histogram_family hk_h_008disc .. hk_h_discstart), so it must reach the
  // bins and never appear in the accepted-and-not-applied note.
  {
    auto j = runCmd(session, "histogram hi, discrete");
    auto g = blockOrEmpty(j, "graph");
    check(blockOrEmpty(j, "error").empty() && g.value("discrete", false) &&
              g.value("note", std::string()).find("discrete") == std::string::npos,
          "W4: `histogram, discrete` is served (the bins change) and is not named as "
          "accepted-and-not-applied");
  }

  // (1) THE SEEDED-RED. Both planes of the multi-layer path.
  {
    auto j = runCmd(session, "twoway (scatter hi x, mcolor(red) bogus(1))");
    auto err = blockOrEmpty(j, "error");
    check(err.value("code", std::string()) == "unsupported_option" &&
              err.value("message", std::string()).find("bogus") != std::string::npos,
          "W4: `twoway (scatter y x, mcolor(red) bogus(1))` is r(198) NAMING `bogus` (rc 0 "
          "before the layer net — the typo ran silently)");
    check(blockOrEmpty(j, "graph").empty(),
          "W4: and no plot is emitted alongside the reject (reject-before-output)");
  }
  {
    auto j = runCmd(session, "twoway (scatter hi x) (line hi x), xlabel(1 2) bogusopt(1)");
    auto err = blockOrEmpty(j, "error");
    check(err.value("code", std::string()) == "unsupported_option" &&
              err.value("message", std::string()).find("bogusopt") != std::string::npos,
          "W4: the GLOBAL option block is netted too (`bogusopt` named; rc 0 before)");
  }
  // ORDERING, and it is the same rule the bare arm's four older legs lock: the
  // net sits AFTER the supported-plot-type pre-scan, so a command-level gap is
  // still reported as one.
  {
    auto err = blockOrEmpty(runCmd(session, "twoway (pcspike hi x lo x, lstyle(ci))"), "error");
    check(err.value("code", std::string()) == "unsupported_command" &&
              err.value("message", std::string()).find("pcspike") != std::string::npos,
          "W4 ordering: an unsupported PLOT TYPE still answers `plot type is not supported`, "
          "not `option lstyle not allowed`");
  }

  // A documented short form resolves. `xlab` reaches xlabel by the prefix rule;
  // `xti` reaches xtitle only through the DECLARED alias, because W4 added
  // `xtick` to the vocabulary and made the prefix ambiguous.
  {
    auto j = runCmd(session, "twoway (scatter hi x), xlab(#5)");
    check(blockOrEmpty(j, "error").empty(),
          "W4: `xlab(#5)` resolves to xlabel (prefix rule) and is accepted");
    auto g = blockOrEmpty(runCmd(session, "scatter hi x, xti(\"gdp\")"), "graph");
    check(g.value("x_label", std::string()) == "gdp",
          "W4: `xti(\"gdp\")` still reaches xtitle — the declared alias covers the prefix "
          "collision `xtick` introduced");
  }

  // The layer interior is canonicalized, so a short spelling reaches layerStyle
  // instead of being accepted by the net and then dropped unread (BUG-233's
  // shape, on a plane the dispatcher's canonicalizer never saw).
  {
    auto layers = blockOrEmpty(runCmd(session, "twoway (connected hi x, lc(navy))"), "graph")
                      .value("layers", nlohmann::json::array());
    check(!layers.empty() && layers[0].value("style", nlohmann::json::object())
                                    .value("color", std::string()) != "#1850a8",
          "W4: `lc(navy)` inside a layer reaches layerStyle (it reads `lcolor`, never `lc`) "
          "instead of being accepted and dropped");
  }

  fs::remove_all(workdir, ec);
}


// BUG-112(7): doexec::executeDo's per-run tempdir reap used to sit as plain
// code AFTER the depth-guarded executeDoSource call, so any exception
// propagating out of executeDoSource (dispatchFn is invoked unguarded at
// doexec.cpp's single call site — see BUG-106's DoExecDepthGuard comment)
// skipped it, leaking the minted tempdir on disk and leaving
// session.runTempdir set for the next top-level run to trip over. Drives the
// REAL doexec::executeDo entry point (not routes::command) with a rigged
// DispatchFn that throws unconditionally, so this is a genuine live-fire of
// the throw path — not a structural stand-in.
// W:P8 (BUG-169). Two graphs are registered under different names; exporting
// each by name must produce DIFFERENT bytes, and exporting an unregistered
// name must fail instead of silently writing the current graph. Before the
// fix all three exports were byte-identical and all three returned rc 0.
void testP8GraphExportByName() {
  std::printf("[selftest] --- W:P8 BUG-169 graph export honors name() ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_p8_gexport").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  piet::SessionState session(workdir);
  auto run = [&](const std::string& cmd) { return piet::dispatch(cmd, session); };

  run("clear");
  run("set obs 40");
  run("generate x = _n");
  run("generate y = _n * _n");
  run("scatter y x, name(gA, replace)");
  run("histogram x, name(gB, replace)");

  const std::string pathA = workdir + "/a.svg";
  const std::string pathB = workdir + "/b.svg";
  const std::string pathGhost = workdir + "/ghost.svg";
  run("graph export \"" + pathA + "\", name(gA) replace");
  run("graph export \"" + pathB + "\", name(gB) replace");
  nlohmann::ordered_json ghost =
      run("graph export \"" + pathGhost + "\", name(gNoSuchGraph) replace");

  auto slurp = [](const std::string& p) {
    std::ifstream f(cecon::to_native_path(p), std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
  };
  const std::string a = slurp(pathA);
  const std::string b = slurp(pathB);
  check(!a.empty(), "BUG-169: `graph export, name(gA)` wrote a file");
  check(!b.empty(), "BUG-169: `graph export, name(gB)` wrote a file");
  check(a != b,
        "BUG-169: two DIFFERENT named graphs export to DIFFERENT bytes (the pre-fix writer "
        "ignored name() and wrote the current graph for both)");

  bool ghostErrored = false;
  for (const auto& blk : ghost) {
    if (blk.is_object() && blk.value("type", std::string()) == "error") ghostErrored = true;
  }
  check(ghostErrored, "BUG-169: exporting an unregistered graph name is an ERROR, not a silent write");
  check(!fs::exists(cecon::to_native_path(pathGhost)),
        "BUG-169: the refused export left NO file behind");
}


// ===========================================================================
// AER scope-2 W2 — the option-vocabulary registration sweep, both ways.
//
// A registration is a CLAIM about a command's option surface, and a claim that
// nothing drives is a comment. Every command dispatcher.cpp's W2 block
// registered gets a pair here:
//
//   NEGATIVE  a token no Stata command has is refused, and the refusal NAMES
//             it. This is what makes `known` falsifiable: a net that answers
//             rc 0 to `zzzfake` is accepting anything, so "x is known" says
//             nothing.
//   POSITIVE  the command's own real options are accepted and the command
//             still produces a block. This is the other half, and it is the
//             half that catches an over-tight registration: a `known` set
//             narrower than the handler's reads turns a working option into a
//             reject, which is the same silent breakage from the other side.
//
// SEVEN COMMANDS HAVE NO NEGATIVE ARM, and that is a measurement, not an
// omission. collapse/file/net/ssc/set/restore/tsfilter have no unknown-option
// net at ALL: measured 2026-09-09 by driving `<cmd> ..., zzzfake` through
// --do, every one answers rc 0 and drops the token silently. Registering their
// vocabulary is what makes that visible to the census; adding a net would be a
// behaviour change W2 does not own. They are pinned below as EXPECTED-SILENT,
// so when W3/W4/W5 wire the net the pin fails and has to be flipped
// deliberately rather than drifting.
void testAerW2OptVocabTwoWay() {
  std::printf("[selftest] --- AER scope-2 W2: option-vocabulary registrations, both ways ---\n");
  namespace fs = std::filesystem;
  std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_aerw2").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // Every block a run produced, flattened: "ERR|<message>" for errors so a
  // refusal and its wording are one string, the block type otherwise.
  auto runOf = [&](const std::vector<std::string>& setup,
                   const std::string& cmd) -> std::pair<std::string, bool> {
    piet::SessionState session(workdir);
    for (const auto& s : setup) (void)piet::dispatch(s, session);
    std::string sig;
    bool sawNonEcho = false;
    for (const auto& b : piet::dispatch(cmd, session)) {
      if (!b.is_object()) continue;
      const std::string t = b.value("type", std::string());
      if (t == "error") {
        sig += "ERR|" + b.value("message", std::string()) + "|";
      } else if (t != "echo") {
        sawNonEcho = true;
      }
    }
    return {sig, sawNonEcho};
  };

  static const std::vector<std::string> kNone = {};
  static const std::vector<std::string> kFatal = {"sysuse fatalities, clear"};
  // A numeric panel id: xtset refuses the string `state` (BUG-1164).
  static const std::vector<std::string> kPanel = {"sysuse fatalities, clear",
                                                  "egen sid = group(state)", "xtset sid year"};
  static const std::vector<std::string> kFit = {"sysuse fatalities, clear",
                                                "quietly regress fatal beertax", "eststo m1"};
  static const std::vector<std::string> kLbl = {"sysuse fatalities, clear",
                                                "label define w2lbl 1 \"a\""};

  struct Leg {
    const std::vector<std::string>* setup;
    std::string positive;  // the command's REAL options, all of them
    std::string negative;  // the same command with one impossible token
    bool netted;           // false = measured to have no unknown-option net
    std::string label;
  };
  const std::string csv = (fs::path(workdir) / "w2.csv").u8string();
  const std::string dta = (fs::path(workdir) / "w2.dta").u8string();
  const std::string txt = (fs::path(workdir) / "w2.txt").u8string();
  const std::vector<Leg> kLegs = {
      // --- promoted BUG-336 nets: the sets did not change, only their home ---
      {&kFatal, "codebook fatal, compact", "codebook fatal, zzzfake", true, "codebook"},
      {&kFatal, "describe", "describe, zzzfake", true, "describe"},
      {&kFatal, "rename fatal fatal2", "rename fatal fatal2, zzzfake", true, "rename"},
      {&kFatal, "generate w2g = 1", "generate w2g = 1, zzzfake", true, "generate"},
      {&kFatal, "sort year", "sort year, zzzfake", true, "sort"},
      {&kFatal, "gsort -year", "gsort -year, zzzfake", true, "gsort"},
      {&kFatal, "compress", "compress, zzzfake", true, "compress"},
      {&kFatal, "recode year (1982/1984 = 1), generate(w2q)",
       "recode year (1982/1984 = 1), zzzfake", true, "recode"},
      {&kFatal, "list fatal in 1/2, noobs clean", "list fatal in 1/2, zzzfake", true, "list"},
      {&kLbl, "label define w2lbl 2 \"b\", add modify", "label define w2lbl 2 \"b\", zzzfake",
       true, "label"},
      {&kLbl, "labelbook", "labelbook, zzzfake", true, "labelbook"},
      {&kFatal, "encode state, generate(w2s) label(w2sl)", "encode state, zzzfake", true,
       "encode"},
      {&kFatal, "expand 2", "expand 2, zzzfake", true, "expand"},
      {&kFatal, "export delimited using \"" + csv + "\", replace nolabel delimiter(comma)",
       "export delimited using \"" + csv + "\", zzzfake", true, "export"},
      {&kFit, "nlcom _b[beertax], level(90)", "nlcom _b[beertax], zzzfake", true, "nlcom"},
      // --- hand-rolled nets: registered here, still rejecting there ---
      {&kFatal, "regress fatal beertax, robust level(90)", "regress fatal beertax, zzzfake",
       true, "regress"},
      {&kPanel, "xtreg fatal beertax, fe", "xtreg fatal beertax, fe zzzfake", true, "xtreg"},
      {&kFatal, "summarize fatal, detail separator(0)", "summarize fatal, zzzfake", true,
       "summarize"},
      {&kFatal, "save \"" + dta + "\", replace", "save \"" + dta + "\", zzzfake", true, "save"},
      {&kFit, "predict w2yh, xb", "predict w2yh, zzzfake", true, "predict"},
      {&kFit, "estimates table m1, b se", "estimates table m1, zzzfake", true, "estimates"},
      {&kPanel, "ivreg2 fatal (beertax = unemp), robust",
       "ivreg2 fatal (beertax = unemp), zzzfake", true, "ivreg2"},
      // --- the graph arm, promoted (W2 registers; W4 implements) ------------
      {&kFatal, "scatter fatal beertax, title(\"t\") xtitle(\"x\")",
       "scatter fatal beertax, zzzfake", true, "scatter"},
      {&kFatal, "histogram fatal, bin(10) freq", "histogram fatal, zzzfake", true, "histogram"},
      {&kFatal, "kdensity fatal, kernel(gaussian)", "kdensity fatal, zzzfake", true, "kdensity"},
      {&kFatal, "twoway histogram fatal, bin(10)", "twoway histogram fatal, zzzfake", true,
       "twoway (umbrella carries the plot-type extras)"},
      // collapse's net landed with collapse_17's option set (by cw fast;
      // w2au_collapse_family.log cz_c_err_opt `nosuchopt` r(198)): the pin flips.
      {&kFatal, "collapse (mean) fatal, by(state)", "collapse (mean) fatal, by(state) zzzfake",
       true, "collapse"},
      // --- registered, and MEASURED to have no net ---------------------------
      {&kFatal, "file open w2h using \"" + txt + "\", write replace",
       "file open w2h using \"" + txt + "\", write replace zzzfake", false, "file"},
      {&kNone, "net query", "net query, zzzfake", false, "net"},
      {&kNone, "ssc install nothingatall", "ssc install nothingatall, zzzfake", false, "ssc"},
      // EM1 (2026-10-21): `set more` has its own arm now and its option net with it
      // (`perm|permanent|permanently`, w2af_macro_text_file.log mt_set_more_*), so
      // this pin flips from "measured to have no net" to "rejects", as instructed.
      {&kNone, "set more off", "set more off, zzzfake", true, "set"},
      // 2026-09-09: the net LANDED (`restore, preserve` needed one), so this pin flips from
      // "measured to have no net" to "rejects" -- exactly what its own comment instructed.
      {&kFatal, "preserve", "restore, zzzfake", true, "restore"},
  };

  for (const auto& leg : kLegs) {
    const auto pos = runOf(*leg.setup, leg.positive);
    // The POSITIVE half. `not allowed` / `does not yet implement` are
    // eval::rejectUnknownOptions' own two wordings (errors.cpp:367-372), so
    // this fails only when the option LAYER refused — an engine error from
    // cobra (a missing binary, say) is a different message and does not.
    check(pos.first.find("not allowed") == std::string::npos &&
              pos.first.find("does not yet implement") == std::string::npos &&
              pos.first.find("is not supported") == std::string::npos,
          "W2 " + leg.label + ": its own real options are accepted (`" + leg.positive +
              "`) -- got " + (pos.first.empty() ? std::string("no error") : pos.first));

    // The NEGATIVE half.
    const auto neg = runOf(*leg.setup, leg.negative);
    if (leg.netted) {
      check(neg.first.find("zzzfake") != std::string::npos,
            "W2 " + leg.label + ": the impossible token is refused BY NAME (`" + leg.negative +
                "`) -- got " +
                (neg.first.empty() ? std::string("rc 0, silently dropped") : neg.first));
    } else {
      check(neg.first.find("zzzfake") == std::string::npos,
            "W2 " + leg.label + ": MEASURED to have no unknown-option net -- `zzzfake` is still "
            "dropped silently. Flip this pin deliberately when the net lands; do not delete it. "
            "Got " + neg.first);
    }
  }

  // --- THE SEEDED-RED SURFACE ------------------------------------------------
  // eval::rejectUnknownOptions counts every abbreviation it has to resolve
  // ITSELF, which can only happen when the dispatcher's resolution point did
  // not cover that net — i.e. when the net's vocabulary is unregistered. The
  // legs below drive one real abbreviation through each net this sweep
  // registered, so deleting any ONE of those registerOptVocab lines makes the
  // count non-zero and this check prints the offending `raw->canonical` pair.
  // Without this block a deleted registration is invisible: every leg above
  // still passes, because the net resolves the key late and answers the same.
  {
    piet::eval::resetOptNetLateResolutionCount();
    const std::vector<std::pair<const std::vector<std::string>*, std::string>> kAbbrev = {
        {&kFatal, "codebook fatal, compa"},                       // codebook  compact
        {&kFatal, "describe, fullname"},                          // describe  fullnames
        {&kFatal, "generate w2ab = 1, befo(year)"},               // generate  before
        {&kFatal, "sort year, stabl"},                            // sort      stable
        {&kFatal, "gsort -year, mfirs"},                          // gsort     mfirst
        {&kFatal, "compress, nocoalesc"},                         // compress  nocoalesce
        {&kFatal, "recode year (1982/1984 = 1), gener(w2ar)"},    // recode    generate
        {&kFatal, "list fatal in 1/2, noob"},                     // list      noobs
        {&kLbl, "label define w2lbl 2 \"b\", modi"},              // label     modify
        {&kFatal, "labelbook, alph"},                             // labelbook alpha
        {&kFatal, "encode state, gener(w2ae)"},                   // encode    generate
        {&kFatal, "expand 2, gene(w2ax)"},                        // expand    generate
        {&kFatal, "export delimited using \"" + csv + "\", repl"},  // export  replace
        {&kFit, "nlcom _b[beertax], lev(90)"},                    // nlcom     level
        {&kFatal, "regress fatal beertax, clus(state)"},          // regress   cluster
        {&kPanel, "xtreg fatal beertax, fe clus(state)"},         // xtreg     cluster
        {&kFatal, "summarize fatal, deta"},                       // summarize detail
        {&kFit, "predict w2ap, resi"},                            // predict   (ambiguous: stays)
        {&kFatal, "scatter fatal beertax, xtit(\"x\")"},          // scatter   xtitle
        {&kFatal, "histogram fatal, freq norma"},                 // histogram normal
        {&kFatal, "joinby state using \"nowhere.dta\", unmatche(master)"},  // joinby unmatched
        {&kFatal, "collapse (mean) fatal, b(state)"},             // collapse  by
    };
    for (const auto& [setup, line] : kAbbrev) (void)runOf(*setup, line);
    check(piet::eval::optNetLateResolutionCount() == 0,
          "W2 GUARD: no net resolved one of these abbreviations itself — a late resolution "
          "means THAT command's registerOptVocab line is missing, so the dispatcher never "
          "canonicalized the key and the handler is comparing raw text again. Last seen: " +
              (piet::eval::optNetLastLateResolution().empty()
                   ? std::string("none")
                   : piet::eval::optNetLastLateResolution()));
    piet::eval::resetOptNetLateResolutionCount();
  }

  // The honest-refusal wording, which is the whole reason `unimplemented` is a
  // separate bucket from "not a member": a real Stata option this build does
  // not have must say so, not read as a typo.
  {
    const auto beta = runOf(kFatal, "regress fatal beertax, beta");
    check(beta.first.find("valid Stata option") != std::string::npos,
          "W2 regress: `beta` is refused as a real-but-unwired Stata option, not as a typo -- "
          "got " + beta.first);
    // joinby's unmatched() is served now; its `update` is the real-but-unwired
    // option this leg probes (joinby's documented `update`).
    const auto unm = runOf(kFatal, "joinby state using \"nowhere.dta\", update");
    check(unm.first.find("valid Stata option") != std::string::npos,
          "W2 joinby: `update` is refused as a real-but-unwired Stata option -- got " +
              unm.first);
    // `be` fits since BUG-1515; `pa` is the xtreg estimator the arm still refuses.
    const auto paq = runOf(kPanel, "xtreg fatal beertax, pa");
    check(paq.first.find("not available") != std::string::npos,
          "W2 xtreg: `pa` keeps its own named refusal (the arm's, not the net's) -- got " +
              paq.first);
  }

  // `d` is BELOW resolveOptionAbbrev's 2-char floor, so only a DECLARED alias
  // can carry it. 160 of the 12 papers' summarize calls spell it that way.
  {
    const piet::eval::OptVocab* v = piet::eval::findOptVocab("summarize");
    check(v != nullptr && v->aliases.count("d") != 0 && v->aliases.at("d") == "detail",
          "W2 summarize: `d` is a declared alias of `detail` (the prefix rule cannot reach it)");
    const auto d = runOf(kFatal, "summarize fatal, d");
    const auto detail = runOf(kFatal, "summarize fatal, detail");
    check(!d.first.empty() == !detail.first.empty() && d.second == detail.second,
          "W2 summarize: `su fatal, d` still behaves as `detail` after the alias landed");
    // The deliberate HOLE, pinned so it cannot be closed by accident. `meanonly`
    // IS read by the summarize arm, and is still absent from `known`: putting it
    // there arms the 2-char prefix rule and makes `su x, mea` accepted, while
    // BUG-264 measured StataBE refusing it (meanonly's minimum is 4). The census
    // therefore reads summarize|meanonly as `unknown`, which is an under-claim,
    // and the fix belongs to the minimums campaign, not to W2. Delete this pin
    // only together with a summarize row in kOptMinComplete.
    check(v != nullptr && v->known.count("meanonly") == 0,
          "W2 summarize: `meanonly` is deliberately NOT in `known` — registering it would "
          "accept `mea`, which the oracle measured as r(198)");
    const auto mea = runOf(kFatal, "summarize fatal, mea");
    check(!mea.first.empty(),
          "W2 summarize: ...and `mea` is therefore still rejected — got " +
              (mea.first.empty() ? std::string("rc 0") : mea.first));
    const auto meanonly = runOf(kFatal, "summarize fatal, meanonly");
    check(meanonly.first.empty(),
          "W2 summarize: ...while the full `meanonly` still runs (the hole is in the CENSUS, "
          "not in the accepted set) -- got " + meanonly.first);
  }

  // B5-D5: `fullnames` is a validated no-op -- the describe block always
  // carries the full, untruncated variable name and the frontend's only
  // renderer sizes its column to the longest name present, so PIET never
  // abbreviates. Measured B4REC|describe|fullname|0| (tests/eval/golden/
  // aer_b4/pins_oracle_r.log:130-131).
  {
    const piet::eval::OptVocab* dv = piet::eval::findOptVocab("describe");
    check(dv != nullptr && dv->known.count("fullnames") != 0,
          "B5-D5 describe: `fullnames` moved from unimplemented to known");
    const auto full = runOf(kFatal, "describe, fullname");
    check(full.first.empty(),
          "B5-D5 describe: `describe, fullname` is accepted (rc 0), not r(198) -- got " +
              full.first);
    // `short` is served since aer_w2 w2b_dataio describe_short (rc 0, r(N) r(k)
    // r(changed), w2b_dataio.log:172-173).
    const auto shortOpt = runOf(kFatal, "describe, short");
    check(shortOpt.first.empty(),
          "B5-D5 describe: `short` is served (rc 0, w2b_dataio describe_short) -- got " +
              shortOpt.first);
  }

  // B5-D6: `sep` is a prefix of BOTH `separator` and `sepby` in kVocabList, so
  // only a DECLARED alias can carry it (same shape as summarize's `d` above).
  {
    const piet::eval::OptVocab* lv = piet::eval::findOptVocab("list");
    check(lv != nullptr && lv->known.count("separator") != 0,
          "B5-D6 list: `separator` moved from unimplemented to known");
    check(lv != nullptr && lv->aliases.count("sep") != 0 && lv->aliases.at("sep") == "separator",
          "B5-D6 list: `sep` is a declared alias of `separator` (ambiguous under the prefix "
          "rule against `sepby`)");
    const auto sep0 = runOf(kFatal, "list fatal in 1/2, sep(0)");
    check(sep0.first.empty(),
          "B5-D6 list: `list ..., sep(0)` is accepted (rc 0), not r(198) -- got " + sep0.first);
    const auto sepBad = runOf(kFatal, "list fatal in 1/2, sep(x)");
    check(!sepBad.first.empty(),
          "B5-D6 list: a non-numeric `sep()` argument is still a named reject, not a silently "
          "accepted garbage value");
  }

  // `graph` is a multiplex, and its registration has to carry the SUBCOMMAND
  // handlers' reads as well as the plot arm's. The first version of this sweep
  // registered only the plot vocabulary, and `as` — graph export's format
  // option — resolved to the plot arm's unimplemented `aspectratio`, so the
  // export handler stopped seeing its own format. Six BS-012/BUG-103 legs went
  // RED. This is the pin that keeps the lesson attached to the registration.
  {
    const piet::eval::OptVocab* g = piet::eval::findOptVocab("graph");
    check(g != nullptr && g->known.count("as") != 0 && g->known.count("replace") != 0 &&
              g->known.count("rows") != 0 && g->known.count("graphregion") != 0,
          "W2 graph: the multiplex's registration carries export/save/combine's own reads "
          "(as/replace/rows/graphregion), not just the plot arm's");
    std::vector<std::pair<std::string, piet::eval::OptValue>> one;
    piet::eval::OptValue pv;
    pv.strValue = "png";
    one.emplace_back("as", pv);
    piet::eval::canonicalizeForCommand(one, "graph");
    check(one.front().first == "as",
          "W2 graph: `as(png)` survives canonicalization intact — it must NOT resolve to "
          "`aspectratio`; got `" + one.front().first + "`");
  }

  fs::remove_all(workdir, ec);
}
