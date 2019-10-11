#include <iostream>
#include "assert.h"

#include "optionparser.h"
#include "smt-switch/msat_factory.h"

#include "bmc.h"
#include "defaults.h"
#include "frontends/btor2_encoder.h"
#include "interpolant.h"
#include "kinduction.h"
#include "printers/btor2_witness_printer.h"
#include "prop.h"
#include "utils/logger.h"

using namespace cosa;
using namespace smt;
using namespace std;

/************************************* Option Handling setup
 * *****************************************/
// from optionparser-1.7 examples -- example_arg.cc
enum optionIndex
{
  UNKNOWN_OPTION,
  HELP,
  ENGINE,
  BOUND,
  PROP,
  VERBOSITY
};

struct Arg : public option::Arg
{
  static void printError(const char * msg1,
                         const option::Option & opt,
                         const char * msg2)
  {
    fprintf(stderr, "%s", msg1);
    fwrite(opt.name, opt.namelen, 1, stderr);
    fprintf(stderr, "%s", msg2);
  }

  static option::ArgStatus Numeric(const option::Option & option, bool msg)
  {
    char * endptr = 0;
    if (option.arg != 0 && strtol(option.arg, &endptr, 10))
    {
    };
    if (endptr != option.arg && *endptr == 0) return option::ARG_OK;

    if (msg) printError("Option '", option, "' requires a numeric argument\n");
    return option::ARG_ILLEGAL;
  }

  static option::ArgStatus NonEmpty(const option::Option & option, bool msg)
  {
    if (option.arg != 0 && option.arg[0] != 0) return option::ARG_OK;

    if (msg)
      printError("Option '", option, "' requires a non-empty argument\n");
    return option::ARG_ILLEGAL;
  }
};

const option::Descriptor usage[] = {
  { UNKNOWN_OPTION,
    0,
    "",
    "",
    Arg::None,
    "USAGE: cosa2 [options] <btor file>\n\n"
    "Options:" },
  { HELP, 0, "", "help", Arg::None, "  --help \tPrint usage and exit." },
  { ENGINE,
    0,
    "e",
    "engine",
    Arg::NonEmpty,
    "  --engine, -e <engine> \tSelect engine from [bmc, ind, int]." },
  { BOUND,
    0,
    "k",
    "bound",
    Arg::Numeric,
    "  --bound, -k \tBound to check up until." },
  { PROP,
    0,
    "p",
    "prop",
    Arg::Numeric,
    "  --prop, -p \tProperty index to check (default: 0)." },
  { VERBOSITY,
    0,
    "v",
    "verbosity",
    Arg::Numeric,
    "  --verbosity, -v \tVerbosity for printing to standard out." },
  { 0, 0, 0, 0, 0, 0 }
};
/*********************************** end Option Handling setup
 * ***************************************/

int main(int argc, char ** argv)
{
  argc -= (argc > 0);
  argv += (argc > 0);  // skip program name argv[0] if present
  option::Stats stats(usage, argc, argv);
  std::vector<option::Option> options(stats.options_max);
  std::vector<option::Option> buffer(stats.buffer_max);
  option::Parser parse(usage, argc, argv, &options[0], &buffer[0]);

  if (parse.error())
  {
    return 1;
  }

  if (options[HELP] || argc == 0)
  {
    option::printUsage(cout, usage);
    return 0;
  }

  if (parse.nonOptionsCount() != 1)
  {
    option::printUsage(cout, usage);
    return 1;
  }

  bool unknown_options = false;
  for (option::Option * opt = options[UNKNOWN_OPTION]; opt; opt = opt->next())
  {
    unknown_options = true;
  }

  if (unknown_options)
  {
    option::printUsage(cout, usage);
    return 1;
  }

  std::string engine = default_engine;
  unsigned int prop_idx = default_prop_idx;
  unsigned int bound = default_bound;
  unsigned int verbosity = default_verbosity;

  for (int i = 0; i < parse.optionsCount(); ++i)
  {
    option::Option & opt = buffer[i];
    switch (opt.index())
    {
      case HELP:
        // not possible, because handled further above and exits the program
      case ENGINE: engine = opt.arg; break;
      case BOUND: bound = atoi(opt.arg); break;
      case PROP: prop_idx = atoi(opt.arg); break;
      case VERBOSITY: verbosity = atoi(opt.arg); break;
      case UNKNOWN_OPTION:
        // not possible because Arg::Unknown returns ARG_ILLEGAL
        // which aborts the parse with an error
        break;
    }
  }

  if (engine != "bmc" && engine != "ind" && engine != "int")
  {
    throw CosaException("Unrecognized engine selection: " + engine);
  }

  // set logger verbosity -- can only be set once
  logger.set_verbosity(verbosity);

  string filename(parse.nonOption(0));

  try
  {
    SmtSolver s;
    SmtSolver second_solver;
    if (engine == "int")
    {
      s = MsatSolverFactory::create_interpolating_solver();
      second_solver = MsatSolverFactory::create();
    }
    else
    {
      s = MsatSolverFactory::create();
      s->set_opt("produce-models", "true");
      s->set_opt("incremental", "true");
    }

    RelationalTransitionSystem rts(s);
    BTOR2Encoder btor_enc(filename, rts);

    // cout << "Created TransitionSystem with:\n";
    // cout << "\t" << rts.inputs().size() << " input variables." << endl;
    // cout << "\t" << rts.states().size() << " state variables." << endl;

    unsigned int num_bad = btor_enc.badvec().size();
    if (prop_idx >= num_bad)
    {
      cout << "Property index " << prop_idx;
      cout << " is greater than the number of bad tags in the btor file (";
      cout << num_bad << ")" << endl;
      return 1;
    }

    Term bad = btor_enc.badvec()[prop_idx];
    Property p(rts, s->make_term(PrimOp::Not, bad));

    std::shared_ptr<Prover> prover;
    if (engine == "bmc")
    {
      prover = std::make_shared<Bmc>(p, s);
    }
    else if (engine == "ind")
    {
      prover = std::make_shared<KInduction>(p, s);
    }
    else if (engine == "int")
    {
      prover = std::make_shared<InterpolantMC>(p, s, second_solver);
    }
    else
    {
      throw CosaException("Unimplemented engine: " + engine);
    }

    ProverResult r = prover->check_until(bound);
    if (r == FALSE)
    {
      cout << "sat" << endl;
      cout << "b" << prop_idx << endl;
      vector<UnorderedTermMap> cex;
      if (prover->witness(cex))
      {
        print_witness_btor(btor_enc, cex);
        // for (size_t j = 0; j < cex.size(); ++j) {
        //   cout << "-------- " << j << " --------" << endl;
        //   const UnorderedTermMap &map = cex[j];
        //   for (auto v : map) {
        //     cout << v.first << " := " << v.second << endl;
        //   }
        // }
      }
      return 1;
    }
    else if (r == TRUE)
    {
      cout << "unsat" << endl;
      cout << "b" << prop_idx << endl;
      return 0;
    }
    else
    {
      cout << "unknown" << endl;
      cout << "b" << prop_idx << endl;
      return 2;
    }
  }
  catch (CosaException & ce)
  {
    cout << ce.what() << endl;
    return 3;
  }

  return 0;
}
