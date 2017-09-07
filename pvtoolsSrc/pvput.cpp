#include <iostream>

#include <stdio.h>
#include <epicsStdlib.h>
#include <epicsGetopt.h>
#include <epicsThread.h>
#include <pv/logger.h>
#include <pv/lock.h>
#include <pv/convert.h>

#include <pv/pvAccess.h>
#include <pva/client.h>
#include <pv/json.h>

#include <vector>
#include <string>
#include <istream>
#include <fstream>
#include <sstream>

#include <pv/pvaDefs.h>
#include <pv/event.h>
#include <epicsExit.h>

#include "pvutils.cpp"

#include <pv/caProvider.h>

using namespace std;
namespace TR1 = std::tr1;
using namespace epics::pvData;
using namespace epics::pvAccess;

namespace {


#define DEFAULT_TIMEOUT 3.0
#define DEFAULT_REQUEST "field(value)"
#define DEFAULT_PROVIDER "pva"

double timeOut = DEFAULT_TIMEOUT;
string request(DEFAULT_REQUEST);
string defaultProvider(DEFAULT_PROVIDER);
const string noAddress;

enum PrintMode { ValueOnlyMode, StructureMode, TerseMode };
PrintMode mode = ValueOnlyMode;

char fieldSeparator = ' ';

bool debug = false;

void usage (bool details=false)
{
    fprintf (stderr,
             "Usage: pvput [options] <PV name> <json_map>\n\n"
             "       pvput [options] <PV name> <field>=<value> ...\n"
             "\n"
             "  -h: Help: Print this message\n"
             "  -v: Print version and exit\n"
             "\noptions:\n"
             "  -r <pv request>:   Request, specifies what fields to return and options, default is '%s'\n"
             "  -w <sec>:          Wait time, specifies timeout, default is %f second(s)\n"
             "  -t:                Terse mode - print only successfully written value, without names\n"
             "  -p <provider>:     Set default provider name, default is '%s'\n"
             "  -q:                Quiet mode, print only error messages\n"
             "  -d:                Enable debug output\n"
             "  -F <ofs>:          Use <ofs> as an alternate output field separator\n"
             "  -f <input file>:   Use <input file> as an input that provides a list PV name(s) to be read, use '-' for stdin\n"
             " enum format:\n"
             "  default: Auto - try value as enum string, then as index number\n"
             "  -n: Force enum interpretation of values as numbers\n"
             "  -s: Force enum interpretation of values as strings\n"
             , DEFAULT_REQUEST, DEFAULT_TIMEOUT, DEFAULT_PROVIDER);
    if(details)
        fprintf (stderr,
                 "\nExamples:\n"
                 "\n"
                 "  pvput double01 1.234\n\n"
                 "equivalent to:\n"
                 "  pvput double01 value=1.234\n\n"
                 );
}


void printValue(std::string const & channelName, PVStructure::const_shared_pointer const & pv)
{
    if (mode == ValueOnlyMode)
    {
        PVField::const_shared_pointer value = pv->getSubField("value");
        if (value.get() == 0)
        {
            std::cerr << "no 'value' field" << std::endl;
            std::cout << std::endl << *(pv.get()) << std::endl << std::endl;
        }
        else
        {
            Type valueType = value->getField()->getType();
            if (valueType != scalar && valueType != scalarArray)
            {
                // special case for enum
                if (valueType == structure)
                {
                    PVStructure::const_shared_pointer pvStructure = TR1::static_pointer_cast<const PVStructure>(value);
                    if (pvStructure->getStructure()->getID() == "enum_t")
                    {
                        if (fieldSeparator == ' ')
                            std::cout << std::setw(30) << std::left << channelName;
                        else
                            std::cout << channelName;

                        std::cout << fieldSeparator;

                        printEnumT(std::cout, pvStructure);

                        std::cout << std::endl;

                        return;
                    }
                }

                // switch to structure mode
                std::cout << channelName << std::endl << *(pv.get()) << std::endl << std::endl;
            }
            else
            {
                if (fieldSeparator == ' ' && value->getField()->getType() == scalar)
                    std::cout << std::setw(30) << std::left << channelName;
                else
                    std::cout << channelName;

                std::cout << fieldSeparator;

                terse(std::cout, value) << std::endl;
            }
        }
    }
    else if (mode == TerseMode)
        terseStructure(std::cout, pv) << std::endl;
    else
        std::cout << std::endl << *(pv.get()) << std::endl << std::endl;
}

struct Putter : public pvac::ClientChannel::PutCallback
{
    epicsEvent wait;
    epicsMutex lock;
    bool done;
    pvac::PutEvent::event_t result;
    std::string message;

    Putter() :done(false) {}

    std::string jblob;

    typedef std::pair<std::string, std::string> KV_t;
    typedef std::vector<KV_t> pairs_t;
    pairs_t pairs;

    virtual void putBuild(const epics::pvData::StructureConstPtr& build, Args& args)
    {
        PVStructurePtr root(getPVDataCreate()->createPVStructure(build));

        if(pairs.empty()) {
            std::istringstream strm(jblob);
            parseJSON(strm, root, &args.tosend);
        } else {
            for(pairs_t::const_iterator it=pairs.begin(), end=pairs.end(); it!=end; ++it)
            {
                PVFieldPtr fld(root->getSubField(it->first));
                if(!fld) {
                    fprintf(stderr, "%s : Error: no such field\n", it->first.c_str());
                } else if(it->second[0]=='{' || it->second[0]=='[') {
                    std::istringstream strm(it->second);
                    parseJSON(strm, fld, &args.tosend);
                } else {
                    PVScalarPtr sfld(std::tr1::dynamic_pointer_cast<PVScalar>(fld));
                    if(!sfld) {
                        fprintf(stderr, "%s : Error: need a scalar field\n", it->first.c_str());
                    } else {
                        sfld->putFrom(it->second);
                        args.tosend.set(sfld->getFieldOffset());
                    }
                }
            }
        }

        args.root = root;
        if(debug)
            std::cout<<"To be sent: "<<args.tosend<<"\n"<<args.root;
    }

    virtual void putDone(const pvac::PutEvent& evt)
    {
        {
            epicsGuard<epicsMutex> G(lock);
            result = evt.event;
            message = evt.message;
            done = true;
        }

        wait.signal();
    }
};

} // namespace

int main (int argc, char *argv[])
{
    int opt;                    /* getopt() current option */
    bool quiet = false;

    istream* inputStream = 0;
    ifstream ifs;
    bool fromStream = false;

    setvbuf(stdout,NULL,_IOLBF,BUFSIZ);    /* Set stdout to line buffering */
    putenv(const_cast<char*>("POSIXLY_CORRECT="));            /* Behave correct on GNU getopt systems; e.g. handle negative numbers */

    while ((opt = getopt(argc, argv, ":hvr:w:tp:qdF:f:ns")) != -1) {
        switch (opt) {
        case 'h':               /* Print usage */
            usage(true);
            return 0;
        case 'v':               /* Print version */
        {
            Version version("pvput", "cpp",
                    EPICS_PVA_MAJOR_VERSION,
                    EPICS_PVA_MINOR_VERSION,
                    EPICS_PVA_MAINTENANCE_VERSION,
                    EPICS_PVA_DEVELOPMENT_FLAG);
            fprintf(stdout, "%s\n", version.getVersionString().c_str());
            return 0;
        }
        case 'w':               /* Set PVA timeout value */
            if((epicsScanDouble(optarg, &timeOut)) != 1 || timeOut <= 0.0)
            {
                fprintf(stderr, "'%s' is not a valid timeout value "
                        "- ignored. ('pvput -h' for help.)\n", optarg);
                timeOut = DEFAULT_TIMEOUT;
            }
            break;
        case 'r':               /* Set PVA timeout value */
            request = optarg;
            // do not override terse mode
            if (mode == ValueOnlyMode) mode = StructureMode;
            break;
        case 't':               /* Terse mode */
            mode = TerseMode;
            break;
        case 'd':               /* Debug log level */
            debug = true;
            break;
        case 'p':               /* Set default provider */
            defaultProvider = optarg;
            break;
        case 'q':               /* Quiet mode */
            quiet = true;
            break;
        case 'F':               /* Store this for output formatting */
            fieldSeparator = (char) *optarg;
            break;
        case 'f':               /* Use input stream as input */
        {
            string fileName = optarg;
            if (fileName == "-")
                inputStream = &cin;
            else
            {
                ifs.open(fileName.c_str(), ifstream::in);
                if (!ifs)
                {
                    fprintf(stderr,
                            "Failed to open file '%s'.\n",
                            fileName.c_str());
                    return 1;
                }
                else
                    inputStream = &ifs;
            }

            fromStream = true;
            break;
        }
        case 'n':
            enumMode = NumberEnum;
            break;
        case 's':
            enumMode = StringEnum;
            break;
        case '?':
            fprintf(stderr,
                    "Unrecognized option: '-%c'. ('pvput -h' for help.)\n",
                    optopt);
            return 1;
        case ':':
            fprintf(stderr,
                    "Option '-%c' requires an argument. ('pvput -h' for help.)\n",
                    optopt);
            return 1;
        default :
            usage();
            return 1;
        }
    }

    if (argc <= optind)
    {
        fprintf(stderr, "No pv name specified. ('pvput -h' for help.)\n");
        return 1;
    }
    string pv = argv[optind++];

    URI uri;
    bool validURI = URI::parse(pv, uri);

    string providerName(defaultProvider);
    string pvName(pv);
    string address(noAddress);
    if (validURI)
    {
        if (uri.path.length() <= 1)
        {
            std::cerr << "invalid URI '" << pv << "', empty path" << std::endl;
            return 1;
        }
        providerName = uri.protocol;
        pvName = uri.path.substr(1);
        address = uri.host;
    }

    int nVals = argc - optind;       /* Remaining arg list are PV names */
    if (nVals > 0)
    {
        // do not allow reading file and command line specified pvs
        fromStream = false;
    }
    else if (nVals < 1 && !fromStream)
    {
        fprintf(stderr, "No value(s) specified. ('pvput -h' for help.)\n");
        return 1;
    }

    vector<string> values;
    if (fromStream)
    {
        string cn;
        while (true)
        {
            *inputStream >> cn;
            if (!(*inputStream))
                break;
            values.push_back(cn);
        }
    }
    else
    {
        // copy values from command line
        for (int n = 0; optind < argc; n++, optind++)
            values.push_back(argv[optind]);
    }

    if(values.empty()) {
        usage();
        fprintf(stderr, "\nNo values provided\n");
        return 1;
    }

    Putter thework;

    if(values[0][0]=='{' && values.size()>1) {
        usage();
        fprintf(stderr, "\nMissing quotes around JSON?\n");
        return 1;
    } else if(values[0][0]=='{') {
        // write entire blob
        thework.jblob = values[0];
    } else {
        for(size_t i=0, N=values.size(); i<N; i++)
        {
            size_t sep = values[i].find_first_of('=');
            if(sep==std::string::npos && i==0 && N==1) {
                // implicit prefix 'value=';
                thework.pairs.push_back(std::make_pair("value", values[i]));
            } else if(sep==std::string::npos && N>1) {
                usage();
                fprintf(stderr, "\nexpected field=value got '%s'\n", values[i].c_str());
                return 1;
            } else {
                thework.pairs.push_back(std::make_pair(values[i].substr(0, sep),
                                                       values[i].substr(sep+1)));
            }
        }
    }

    PVStructure::shared_pointer pvRequest;
    try {
        pvRequest = createRequest(request);
    } catch(std::exception& e){
        fprintf(stderr, "failed to parse request string: %s\n", e.what());
        return 1;
    }

    SET_LOG_LEVEL(debug ? logLevelDebug : logLevelError);

    std::cout << std::boolalpha;
    terseSeparator(fieldSeparator);
    setEnumPrintMode(enumMode);

    epics::pvAccess::ca::CAClientFactory::start();

    pvac::ClientProvider ctxt(providerName);

    pvac::ClientChannel chan(ctxt.connect(pvName));

    if (mode != TerseMode && !quiet) {
        std::cout << "Old : ";
        printValue(pvName, chan.get(timeOut, pvRequest));
    }

    {
        pvac::Operation op(chan.put(&thework, pvRequest));

        epicsGuard<epicsMutex> G(thework.lock);
        while(!thework.done) {
            epicsGuardRelease<epicsMutex> U(G);
            if(!thework.wait.wait(timeOut)) {
                fprintf(stderr, "Put timeout\n");
                return 1;
            }
        }
    }

    if(thework.result==pvac::PutEvent::Fail) {
        fprintf(stderr, "Error: %s\n", thework.message.c_str());
    }

    if (mode != TerseMode && !quiet) {
        std::cout << "New : ";
    }
    printValue(pvName, chan.get(timeOut, pvRequest));

    return thework.result!=pvac::PutEvent::Success;
}
