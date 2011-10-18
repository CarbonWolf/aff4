import SCons
import os
import re
import template
import urllib

########################################################
# Templates for a single test suite
########################################################
__cuTestSuiteTmpl = r"""/* Autogenerated from %(filename)s */
#include <CUnit/CUnit.h>
#include <stdlib.h>

#define INIT()      static int __initSuite (void)
#define CLEAN()     static int __cleanSuite (void)
#define TEST(name)  static void name (void)

/*********************************************************************
 * BEGIN %(filename)s
 ********************************************************************/
#line 1 "%(fullname)s"
%(filecontent)s
/*********************************************************************
 * END %(filename)s
 ********************************************************************/

/* Create the test suite */
#ifdef __cplusplus
extern "C"
#endif
CU_ErrorCode create_%(suitename)s_suite (void)
{
   CU_pSuite suite = CU_add_suite ("%(suitename)s", %(initSuite)s, %(cleanSuite)s);
   if (suite == NULL) return CU_get_error();

%(addTests (tests))s

   return CUE_SUCCESS;
}"""
__cuAddTestTmpl = """   if (NULL == CU_add_test (suite, "%(test)s", %(test)s)) return CU_get_error();"""

########################################################
# Wrap a test source file to build a test suite
########################################################
__cleanRegexp = re.compile (r"CLEAN\s*\(\s*\)\s*\{", re.M)
__initRegexp = re.compile (r"INIT\s*\(\s*\)\s*\{", re.M)
__testRegexp = re.compile (r"TEST\s*\(\s*(\w+)\s*\)\s*\{", re.M)
def __makeCUTestSuite (target, source, env):
    # Sanity check
    if len (target) != 1 or len (source) != 1:
        raise SCons.Errors.BuildError, "Must have exactly one source and one target"

    target = target[0]
    source = source[0]

    # Read the input file
    inp = source.get_contents().replace ("\r\n", "\n")

    # Create the template values dictionary
    values = {
        "addTests": lambda tests: "\n".join ([ __cuAddTestTmpl % { "test": t } for t in tests ]),
        "filecontent": inp,
        "filename": urllib.pathname2url (os.path.basename (str (source))),
        "fullname": urllib.pathname2url (str (source)),
        "suitename": env["CU_SUITE_NAME"],
        "tests": __testRegexp.findall (inp)
        }

    # Find the initialization and cleanup functions
    if __initRegexp.search (inp):
        values["initSuite"] = "__initSuite"
    else:
        values["initSuite"] = "NULL"
    if __cleanRegexp.search (inp):
        values["cleanSuite"] = "__cleanSuite"
    else:
        values["cleanSuite"] = "NULL"

    # Write the file
    out = open (str (target), "w")
    print >>out, __cuTestSuiteTmpl % template.Eval (values)

    # Done
    out.close()

########################################################
# Templates for the main CUnit source file
########################################################

__cuMainTmpl = r"""/* File auto-generated by the SCons scripts */
#include <CUnit/CUnit.h>
#include <CUnit/Automated.h>
#include <CUnit/Basic.h>
#include <CUnit/Console.h>
#ifdef HAVE_CURSES
#include <CUnit/CUCurses.h>
#endif

#include <stdio.h>
#include <stdlib.h>

typedef enum {
   CU_MODE_NOT_SET,
   CU_MODE_AUTO,
   CU_MODE_BASIC,
   CU_MODE_CONSOLE
#ifdef HAVE_CURSES
   , CU_MODE_CURSES
#endif
} CuMode;


char TEMP_DIR[1024] = "/tmp/";

/* Test suites declarations */
%(declareSuites (suites))s

%(mainDeclares (env))s

/* Program entry point */
int main (int argc, char* argv[])
{
   CuMode mode = CU_MODE_NOT_SET;
   //talloc_enable_leak_report_full();
   CU_initialize_registry();

   /* Make some temporary place to write files. */
   strcat(TEMP_DIR,"aff4_test_XXXXXX");
   mkdtemp(TEMP_DIR);

%(mainInit (env))s

   /* initialize the CUnit test registry */
   if (CU_initialize_registry() != CUE_SUCCESS)
      return CU_get_error();

   /* Create the test suites */
%(addSuites (suites))s

   if (mode == CU_MODE_NOT_SET) {
      mode = CU_MODE_BASIC;

      /* Parse arguments */
      if (argc > 3 || argc < 2) {
         usage (argv[0]);
         exit (1);
      }

      if (argc == 3) {
        char *suite_name = argv[1];
        char *test_name = argv[2];

        CU_pSuite suite = CU_get_suite_by_name(suite_name,
                                               CU_get_registry());

        CU_pTest test = CU_get_test_by_name(test_name, suite);
        CU_basic_set_mode(CU_BRM_VERBOSE);

#if 0
// This does not seem to work for some reason.
        CU_basic_run_suite(suite);
        printf ("\n");
#endif
        test->pTestFunc ();

        CU_basic_show_failures (CU_get_failure_list());
        printf ("\n\n");

        exit(0);
      } else if (argc == 2) {
         if (strcmp (argv[1], "auto") == 0)
            mode = CU_MODE_AUTO;
         else if (strcmp (argv[1], "basic") == 0)
            mode = CU_MODE_BASIC;
         else if (strcmp (argv[1], "console") == 0)
            mode = CU_MODE_CONSOLE;
         else if (strcmp (argv[1], "list") == 0) {
            CU_pSuite suite;
            CU_pTest test;
            CU_pTestRegistry registry = CU_get_registry();

            printf("Test Suite\t\ttest\n");
            printf("==========\t\t====\n");
            for(suite=registry->pSuite; suite; suite = suite->pNext) {
              for(test=suite->pTest; test; test = test -> pNext) {
                  printf("%%s\t\t%%s\n", suite->pName, test->pName);
              };
            };
            exit(0);
         }
#ifdef HAVE_CURSES
         else if (strcmp (argv[1], "curses") == 0)
            mode = CU_MODE_CURSES;
#endif
         else {
            usage (argv[0]);
            if (strcmp (argv[1], "help") == 0)
               exit (0);
            else
               exit (1);
         }
      }
   }

   /* Run the tests */
   switch (mode) {
   case CU_MODE_NOT_SET:
      fprintf (stderr, "Fatal error: should not get here!\n");
      exit (1);

   case CU_MODE_AUTO:
      CU_automated_run_tests();
      break;

   case CU_MODE_BASIC:
      CU_basic_run_tests();
      printf ("\n");
      CU_basic_show_failures (CU_get_failure_list());
      printf ("\n\n");
      break;

   case CU_MODE_CONSOLE:
      CU_console_run_tests();
      break;

#ifdef HAVE_CURSES
   case CU_MODE_CURSES:
      CU_curses_run_tests();
      break;
#endif
   }

   /* Cleanup */
   CU_cleanup_registry();
   return CU_get_error();
}"""
__cuDeclareSuiteTmpl = r"CU_ErrorCode create_%(suite)s_suite (void);"
__cuAddSuiteTmpl = r"""
   if (create_%(suite)s_suite() != CUE_SUCCESS) {
      CU_cleanup_registry();
      return CU_get_error();
   }"""

def mainDeclares (env):
    decFile = env.get ("CU_MAIN_DECLARES_FILE", None)
    decText = env.get ("CU_MAIN_DECLARES", None)
    if decFile is not None:
        return '#include "%s"\n' % decFile
    elif decText is not None:
        return decText
    else:
        return r"""
/* Print an help message */
void usage (char* name)
{
#ifdef HAVE_CURSES
   fprintf (stderr, "Usage: %s [ auto | basic | console | list | curses | help ]\n", name);
#else
   fprintf (stderr, "Usage: %s [ auto | basic | console | list | help ]\n", name);
#endif
}
"""

def mainInit (env):
    iniFile = env.get ("CU_MAIN_INIT_FILE", None)
    iniText = env.get ("CU_MAIN_INIT", None)
    if iniFile is not None:
        return '#include "%s"\n' % iniFile
    elif iniText is not None:
        return iniText
    else:
        return ""

########################################################
# Create the main CUnit source file
########################################################
def __makeCUMainSource (target, source, env):
    # Sanity checks
    if len (target) != 1:
        raise SCons.Errors.BuildError, "Must have exactly one target"
    if len (source) != 1:
        raise SCons.Errors.BuildError, "Must have exactly one source"

    # Get the list of suites and the main template
    tmplFile = env.get ("CU_MAIN_TEMPLATE_FILE", None)
    tmpl = env.get ("CU_MAIN_TEMPLATE", None)
    if tmplFile:
        tmpl = env.File (tmplFile).get_contents()
    elif tmpl is None:
        tmpl = __cuMainTmpl
    suites = source[0].read()

    # Create the template values dictionary
    values = {
        "addSuites": lambda suites: "\n".join ([ __cuAddSuiteTmpl % { "suite": s } for s in suites ]),
        "declareSuites": lambda suites: "\n".join ([ __cuDeclareSuiteTmpl % { "suite": s } for s in suites ]),
        "env": env,
        "mainDeclares": mainDeclares,
        "mainInit": mainInit,
        "suites": suites
        }

    # Write the file
    out = open (str (target[0]), "w")
    print >>out, tmpl % template.Eval (values)

    # Done
    out.close()

def __emitCUMainSource (target, source, env):
    "Adds implicit dependencies for the main source file"

    for var in [ "CU_MAIN_DECLARES",
                 "CU_MAIN_INIT" ]:
        if env.has_key ("%s_FILE" % var):
            env.Depends (target, env.Value (env.subst ("$%s_FILE" % var)))
        else:
            env.Depends (target, env.Value (env.subst ("$%s" % var)))
    if env.has_key ("CU_MAIN_TEMPLATE_FILE"):
        env.Depends (target, "$CU_MAIN_TEMPLATE_FILE")
    else:
        env.Depends (target, env.Value (env.subst ("$CU_MAIN_TEMPLATE")))
    return (target, source)

########################################################
# Cleanup coverage data files associated with the target
########################################################
def __cleanupCoverageData (target, source, env):
    # Note that the target must have a cuObjects field!
    for o in target[0].cuObjects:
        name = "%s.gcda" % (os.path.splitext (o.abspath)[0])
        if os.path.exists (name):
            os.unlink (name)

########################################################
# Create a CUnit test program
########################################################
def buildCUnitTestFromFiles (env,
                             files,
                             buildFolder = "",
                             vars = None,
                             extraObjects = [],
                             package="cunit",
                             **kwargs):
    # Create the Builders
    env.Append (BUILDERS = {
        "CUTestSuite":
        env.Builder (action = env.Action (__makeCUTestSuite, "$$CUTESTSUITESTR"),
                     prefix = "cunit_",
                     single_source = True),

        "CUMainSource":
        env.Builder (action = env.Action (__makeCUMainSource,
                                          "$$CUMAINSOURCESTR"),
                     emitter = __emitCUMainSource)
        },
                CUTESTSUITESTR = \
                ("Creating source for test suite '${CU_SUITE_NAME}'..."),
                CUMAINSOURCESTR = \
                ("Creating main CUnit source file..."))
    # Create the source files for the suites
    suites = set()
    sources = []
    for source in files:
        path, base = os.path.split (source)
        suiteName = os.path.splitext (base)[0]
        sources.append (env.CUTestSuite (os.path.join (buildFolder, "cunit_%s" % base),
                                         source,
                                         CU_SUITE_NAME = suiteName,
                                         **kwargs))
        suites.add (suiteName)
    # Create the main source file
    sources.append (env.CUMainSource (os.path.join (buildFolder, "cutest_main.c"),
                                      env.Value (suites), **kwargs))

    # Build the test program
    from SCons.Util import CLVar
    libs = kwargs.get ("LIBS", None)
    if libs:
        del kwargs["LIBS"]
        libs = CLVar (libs) + "cunit"
    else:
        libs = CLVar (env.get ("LIBS", [])) + "cunit"
    cutest = env.Program (os.path.join (buildFolder, package + "-test"),
                          sources + extraObjects,
                          LIBS = libs,
                          **kwargs)[0]

    # Cleanup the coverage data.
    # - *.gcno files are generated along with the object files and
    #   contain function information. They must be cleaned along with
    #   the associated object;
    # - *.gcda files are generated when running the program. They must
    #   be deleted each time the program is rebuilt, and cleaned along
    #   with the related object.
    if env.get("coverage"):
        env["CLEANUP_COVERAGE_DATA_STR"] = \
            ("Cleaning coverage data for ${TARGET.name}...")
        objects = env.Flatten (sources + extraObjects)
        cutest.cuObjects = objects
        env.AddPreAction (cutest,
                          env.Action (__cleanupCoverageData,
                                      "$$CLEANUP_COVERAGE_DATA_STR"))
        for o in objects:
            baseName = os.path.splitext (o.path)[0]
            env.Clean (o, "#%s.gcda" % baseName)
            env.Clean (o, "#%s.gcno" % baseName)
            env.SideEffect ("#%s.gcno" % baseName, o)

    return cutest

def buildCUnitTest (env,
                    sourceFolder,
                    buildFolder = "",
                    vars = None,
                    **kwargs):
    return buildCUnitTestFromFiles (env,
                                    [ os.path.join (sourceFolder, source)
                                      for source in tools.scanFolder (sourceFolder) ],
                                    buildFolder,
                                    vars,
                                    **kwargs)
