#include "Phase1Trace.h"

#include <iomanip>
#include <stdexcept>

namespace DRAMSim
{
namespace
{
std::string csvField(const std::string& value)
{
    if (value.find_first_of(",\"\n\r") == std::string::npos)
        return value;
    std::string quoted = "\"";
    for (char c : value)
    {
        if (c == '"')
            quoted += '"';
        quoted += c;
    }
    quoted += '"';
    return quoted;
}

std::string optionalInt(int value)
{
    if (value < 0)
        return "";
    return std::to_string(value);
}
}  // namespace

Phase1Trace::Phase1Trace(const std::string& path, bool enabled) : enabled_(enabled)
{
    if (path.empty())
        throw std::invalid_argument("Phase1Trace path is empty");
    output_.open(path, std::ios::out | std::ios::trunc);
    if (!output_)
        throw std::runtime_error("cannot open Phase1Trace: " + path);
    output_ << "event_seq,cycle,phase,event_type,kernel_id,version_id,access_id,channel,rank,"
               "actual_bank,row,column,access_path,bytes,status\n";
    output_.flush();
}

Phase1Trace::~Phase1Trace()
{
    if (output_)
    {
        output_.flush();
        output_.close();
    }
}

void Phase1Trace::event(uint64_t cycle, const std::string& phase, const std::string& eventType,
                        const std::string& kernelId, const std::string& versionId,
                        const std::string& accessId, int channel, int rank, int actualBank, int row,
                        int column, const std::string& accessPath, int bytes,
                        const std::string& status)
{
    if (!enabled_)
        return;
    ++sequence_;
    output_ << sequence_ << ',' << cycle << ',' << csvField(phase) << ',' << csvField(eventType)
            << ',' << csvField(kernelId) << ',' << csvField(versionId) << ','
            << csvField(accessId) << ',' << optionalInt(channel) << ',' << optionalInt(rank) << ','
            << optionalInt(actualBank) << ',' << optionalInt(row) << ',' << optionalInt(column)
            << ',' << csvField(accessPath) << ',' << optionalInt(bytes) << ','
            << csvField(status) << '\n';
    output_.flush();
}

}  // namespace DRAMSim
