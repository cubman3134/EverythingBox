/* A shared library that loads fine and is NOT a libretro core (issue #98).
 *
 * The failure this covers is the commonest one a user will hit: they point the picker at a DLL that is not a
 * core at all — a game's own dependency, a Qt library, something they downloaded next to the core. That must
 * come back as a sentence naming the file, not as a null call inside the frontend. Written as source and
 * built in-tree, like the core fixture beside it, so nothing binary is committed.
 */
#if defined(_WIN32)
#  define EB_FIXTURE_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#  define EB_FIXTURE_EXPORT __attribute__((visibility("default")))
#else
#  define EB_FIXTURE_EXPORT
#endif

EB_FIXTURE_EXPORT int eb_fixture_not_a_core(void)
{
   return 98;
}
