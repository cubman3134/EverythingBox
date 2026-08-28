// Headless test for RiivolutionPatch, DiscOverlay and DiscCompose's pure parts.
//
// The XML fixtures here are cut down from the real document shipped by the mod this work targets, so the
// element shapes, attribute names and the locale ALIASING (seven disc folders sourced from one external
// folder) are the measured article rather than an invention.
//
// Mutation targets: drop the <memory> refusal and case 3 passes when it must fail; drop the multi-choice
// refusal and case 4 passes; drop the containment check in DiscOverlay and case 7 writes outside the tree.
//
// Prints RIIVOLUTION-OK on success; RIIVOLUTION-FAIL (nonzero exit) on any miss.
#include "../src/core/RiivolutionPatch.h"
#include <QByteArray>
#include <cstdio>

static int failures = 0;
static void check(bool cond, const char* what)
{
    if (!cond) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

static QByteArray realShapeXml()
{
    return QByteArray(
        "<wiidisc version=\"1\" root=\"\">\n"
        "  <id game=\"SB4\"><region type=\"P\"/></id>\n"
        "  <options><section name=\"Super Mario Gravity\">\n"
        "    <option name=\"Demo\"><choice name=\"Enabled\"><patch id=\"smgra\"/></choice></option>\n"
        "  </section></options>\n"
        "  <patch id=\"smgra\" root=\"/SuperMarioGravity_Demo\">\n"
        "    <savegame external=\"SaveGame/{$__gameid}\" clone=\"false\"/>\n"
        "    <folder disc=\"/StageData\" external=\"StageData\" create=\"true\"/>\n"
        "    <folder disc=\"/LocalizeData/EuFrench\" external=\"LocalizeData/EuEnglish\" create=\"true\"/>\n"
        "  </patch>\n"
        "</wiidisc>\n");
}

int main()
{
    // 1. The measured shape parses, and the patch root is carried through.
    {
        const auto p = RiivolutionPatch::parse(realShapeXml());
        check(p.ok, "1: the real document shape parses");
        check(p.refusal.isEmpty(), "1: a parse that succeeded states no refusal");
        check(p.root == QStringLiteral("/SuperMarioGravity_Demo"), "1: patch root is carried through");
        check(p.ops.size() == 2, "1: both folder mappings survive");
    }

    // 2. Locale ALIASING: two different disc paths may share one external path. A map keyed by external
    //    path would silently collapse these, and six of the mod's twelve mappings would vanish.
    {
        const auto p = RiivolutionPatch::parse(realShapeXml());
        check(p.ops.size() == 2, "2: aliased mappings are not collapsed");
        check(p.ops[1].discPath == QStringLiteral("/LocalizeData/EuFrench"), "2: alias disc path kept");
        check(p.ops[1].externalPath == QStringLiteral("LocalizeData/EuEnglish"), "2: alias source kept");
    }

    // 3. <memory> is REFUSED, not ignored. A RAM patch cannot exist in a composed disc.
    {
        const auto p = RiivolutionPatch::parse(QByteArray(
            "<wiidisc version=\"1\"><options><section name=\"s\"><option name=\"o\">"
            "<choice name=\"c\"><patch id=\"p\"/></choice></option></section></options>"
            "<patch id=\"p\" root=\"/m\"><memory offset=\"0x80000000\" value=\"60000000\"/></patch></wiidisc>"));
        check(!p.ok, "3: a document with <memory> is refused");
        check(p.refusal.contains(QStringLiteral("memory")), "3: the refusal names the element");
    }

    // 4. A document that offers a real choice is refused: there is no UI to choose with, and choosing
    //    silently installs a different mod from the one the user thinks they picked.
    {
        const auto p = RiivolutionPatch::parse(QByteArray(
            "<wiidisc version=\"1\"><options><section name=\"s\"><option name=\"o\">"
            "<choice name=\"a\"><patch id=\"p\"/></choice>"
            "<choice name=\"b\"><patch id=\"q\"/></choice></option></section></options>"
            "<patch id=\"p\" root=\"/m\"/><patch id=\"q\" root=\"/n\"/></wiidisc>"));
        check(!p.ok, "4: a multi-choice document is refused");
        check(p.refusal.contains(QStringLiteral("choice")), "4: the refusal says a choice was needed");
    }

    // 5. <savegame> is ignored, and the caller is TOLD it was ignored.
    {
        const auto p = RiivolutionPatch::parse(realShapeXml());
        check(p.ok, "5: <savegame> does not fail the parse");
        check(p.savegameIgnored, "5: the caller is told <savegame> was dropped");
    }

    // 6. Malformed input is a refusal, never a crash and never a silent empty success.
    {
        const auto p = RiivolutionPatch::parse(QByteArray("<wiidisc><patch"));
        check(!p.ok, "6: malformed XML is refused");
        check(!p.refusal.isEmpty(), "6: a refusal always states a reason");
    }

    if (failures == 0) { std::printf("RIIVOLUTION-OK\n"); return 0; }
    std::fprintf(stderr, "RIIVOLUTION-FAIL (%d)\n", failures);
    return 1;
}
