/**************************************************************************
 *
 * Copyright 2010 VMware, Inc.
 * Copyright 2011 Intel corporation
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 **************************************************************************/

#include <sstream>
#include <string.h>
#include <limits.h> // for CHAR_MAX
#include <getopt.h>

#include <set>

#include "cli.hpp"

#include "os_string.hpp"

#include "trace_analyzer.hpp"
#include "trace_callset.hpp"
#include "trace_parser.hpp"
#include "trace_writer.hpp"

static const char *synopsis = "Create a new trace by trimming an existing trace.";

static void
usage(void)
{
    std::cout
        << "usage: apitrace trim [OPTIONS] TRACE_FILE...\n"
        << synopsis << "\n"
        "\n"
        "    -h, --help               Show detailed help for trim options and exit\n"
        "        --calls=CALLSET      Include specified calls in the trimmed output.\n"
        "        --frames=FRAMESET    Include specified frames in the trimmed output.\n"
        "        --deps               Include additional calls to satisfy dependencies\n"
        "        --prune              Omit uninteresting calls from the trace output\n"
        "    -a, --auto               Trim automatically to calls specified in --calls/--frames\n"
        "                             Equivalent to both --deps and --prune\n"
        "        --print-callset      Print the final set of calls included in output\n"
        "        --thread=THREAD_ID   Only retain calls from specified thread\n"
        "    -o, --output=TRACE_FILE  Output trace file\n"
    ;
}

static void
help()
{
    std::cout
        << "usage: apitrace trim [OPTIONS] TRACE_FILE...\n"
        << synopsis << "\n"
        "\n"
        "    -h, --help               Show this help message and exit\n"
        "\n"
        "        --calls=CALLSET      Include specified calls in the trimmed output.\n"
        "        --frames=FRAMESET    Include specified frames in the trimmed output.\n"
        "\n"
        "        --deps               Perform dependency analysis and include dependent\n"
        "                             calls as needed, (even if those calls were not\n"
        "                             explicitly requested with --calls or --frames).\n"
        "\n"
        "        --prune              Omit calls with no side effects, even if the call\n"
        "                             is within the range specified by --calls/--frames.\n"
        "\n"
        "    -a, --auto               Use dependency analysis and pruning\n"
        "                             of uninteresting calls the resulting trace may\n"
        "                             include more and less calls than specified.\n"
        "                             This option is equivalent\n"
        "                             to passing both --deps and --prune.\n"
        "\n"
        "        --print-callset      Print to stdout the final set of calls included\n"
        "                             in the trim output. This can be useful for\n"
        "                             tweaking trimmed callset from --auto on the\n"
        "                             command-line.\n"
        "                             Use --calls=@FILE to read callset from a file.\n"
        "\n"
        "        --thread=THREAD_ID   Only retain calls from specified thread\n"
        "\n"
        "    -o, --output=TRACE_FILE  Output trace file\n"
        "\n"
    ;
}

enum {
    CALLS_OPT = CHAR_MAX + 1,
    FRAMES_OPT,
    DEPS_OPT,
    PRUNE_OPT,
    THREAD_OPT,
    PRINT_CALLSET_OPT,
};

const static char *
shortOptions = "aho:x";

const static struct option
longOptions[] = {
    {"help", no_argument, 0, 'h'},
    {"calls", required_argument, 0, CALLS_OPT},
    {"frames", required_argument, 0, FRAMES_OPT},
    {"deps", no_argument, 0, DEPS_OPT},
    {"prune", no_argument, 0, PRUNE_OPT},
    {"auto", no_argument, 0, 'a'},
    {"thread", required_argument, 0, THREAD_OPT},
    {"output", required_argument, 0, 'o'},
    {"print-callset", no_argument, 0, PRINT_CALLSET_OPT},
    {0, 0, 0, 0}
};

struct stringCompare {
    bool operator() (const char *a, const char *b) const {
        return strcmp(a, b) < 0;
    }
};

struct trim_options {
    /* Calls to be included in trace. */
    trace::CallSet calls;

    /* Frames to be included in trace. */
    trace::CallSet frames;

    /* Whether dependency analysis should be performed. */
    bool dependency_analysis;

    /* Whether uninteresting calls should be pruned.. */
    bool prune_uninteresting;

    /* Output filename */
    std::string output;

    /* Emit only calls from this thread (-1 == all threads) */
    int thread;

    /* Print resulting callset */
    int print_callset;
};

static int
trim_trace(const char *filename, struct trim_options *options)
{
    trace::ParseBookmark beginning;
    trace::Parser p;
    TraceAnalyzer analyzer;
    std::set<unsigned> *required;
    unsigned frame;
    int call_range_first, call_range_last;

    if (!p.open(filename)) {
        std::cerr << "error: failed to open " << filename << "\n";
        return 1;
    }

    /* Mark the beginning so we can return here for pass 2. */
    p.getBookmark(beginning);

    /* In pass 1, analyze which calls are needed. */
    frame = 0;
    trace::Call *call;
    while ((call = p.parse_call())) {

        /* There's no use doing any work past the last call or frame
         * requested by the user. */
        if (call->no > options->calls.getLast() ||
            frame > options->frames.getLast()) {
            
            delete call;
            break;
        }

        /* If requested, ignore all calls not belonging to the specified thread. */
        if (options->thread != -1 && call->thread_id != options->thread) {
            goto NEXT;
        }

        /* Also, prune if uninteresting (unless the user asked for no pruning. */
        if (options->prune_uninteresting && call->flags & trace::CALL_FLAG_VERBOSE) {
            goto NEXT;
        }

        /* If this call is included in the user-specified call set,
         * then require it (and all dependencies) in the trimmed
         * output. */
        if (options->calls.contains(*call) ||
            options->frames.contains(frame, call->flags)) {

            analyzer.require(call);
        }

        /* Regardless of whether we include this call or not, we do
         * some dependency tracking (unless disabled by the user). We
         * do this even for calls we have included in the output so
         * that any state updates get performed. */
        if (options->dependency_analysis) {
            analyzer.analyze(call);
        }

    NEXT:
        if (call->flags & trace::CALL_FLAG_END_FRAME)
            frame++;

        delete call;
    }

    /* Prepare output file and writer for output. */
    if (options->output.empty()) {
        os::String base(filename);
        base.trimExtension();

        options->output = std::string(base.str()) + std::string("-trim.trace");
    }

    trace::Writer writer;
    if (!writer.open(options->output.c_str())) {
        std::cerr << "error: failed to create " << filename << "\n";
        return 1;
    }

    /* Reset bookmark for pass 2. */
    p.setBookmark(beginning);

    /* In pass 2, emit the calls that are required. */
    required = analyzer.get_required();

    frame = 0;
    call_range_first = -1;
    call_range_last = -1;
    while ((call = p.parse_call())) {

        /* There's no use doing any work past the last call or frame
         * requested by the user. */
        if (call->no > options->calls.getLast() ||
            frame > options->frames.getLast()) {

            break;
        }

        if (required->find(call->no) != required->end()) {
            writer.writeCall(call);

            if (options->print_callset) {
                if (call_range_first < 0) {
                    call_range_first = call->no;
                    printf ("%d", call_range_first);
                } else if (call->no != call_range_last + 1) {
                    if (call_range_last != call_range_first)
                        printf ("-%d", call_range_last);
                    call_range_first = call->no;
                    printf (",%d", call_range_first);
                }
                call_range_last = call->no;
            }
        }

        if (call->flags & trace::CALL_FLAG_END_FRAME) {
            frame++;
        }

        delete call;
    }

    if (options->print_callset) {
        if (call_range_last != call_range_first)
            printf ("-%d\n", call_range_last);
    }

    std::cerr << "Trimmed trace is available as " << options->output << "\n";

    return 0;
}

static int
command(int argc, char *argv[])
{
    struct trim_options options;

    options.calls = trace::CallSet(trace::FREQUENCY_NONE);
    options.frames = trace::CallSet(trace::FREQUENCY_NONE);
    options.dependency_analysis = false;
    options.prune_uninteresting = false;
    options.output = "";
    options.thread = -1;
    options.print_callset = 0;

    int opt;
    while ((opt = getopt_long(argc, argv, shortOptions, longOptions, NULL)) != -1) {
        switch (opt) {
        case 'h':
            help();
            return 0;
        case CALLS_OPT:
            options.calls = trace::CallSet(optarg);
            break;
        case FRAMES_OPT:
            options.frames = trace::CallSet(optarg);
            break;
        case DEPS_OPT:
            options.dependency_analysis = true;
            break;
        case PRUNE_OPT:
            options.prune_uninteresting = true;
            break;
        case 'a':
            options.dependency_analysis = true;
            options.prune_uninteresting = true;
            break;
        case THREAD_OPT:
            options.thread = atoi(optarg);
            break;
        case 'o':
            options.output = optarg;
            break;
        case PRINT_CALLSET_OPT:
            options.print_callset = 1;
            break;
        default:
            std::cerr << "error: unexpected option `" << opt << "`\n";
            usage();
            return 1;
        }
    }

    /* If neither of --calls nor --frames was set, default to the
     * entire set of calls. */
    if (options.calls.empty() && options.frames.empty()) {
        options.calls = trace::CallSet(trace::FREQUENCY_ALL);
    }

    if (optind >= argc) {
        std::cerr << "error: apitrace trim requires a trace file as an argument.\n";
        usage();
        return 1;
    }

    if (argc > optind + 1) {
        std::cerr << "error: extraneous arguments:";
        for (int i = optind + 1; i < argc; i++) {
            std::cerr << " " << argv[i];
        }
        std::cerr << "\n";
        usage();
        return 1;
    }

    if (options.dependency_analysis) {
        std::cerr <<
            "Note: The dependency analysis in \"apitrace trim\" is still experimental.\n"
            "      We hope that it will be useful, but it may lead to incorrect results.\n"
            "      If you find a trace that misbehaves while trimming, please share that\n"
            "      by sending email to apitrace@lists.freedesktop.org, cworth@cworth.org\n";
    }

    return trim_trace(argv[optind], &options);
}

const Command trim_command = {
    "trim",
    synopsis,
    help,
    command
};
