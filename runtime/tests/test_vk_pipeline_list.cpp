// The Vulkan pipeline list's line format (vk_pipelines.h): Metal's lines with the VkFormat number as the format field.
#include "check.h"
#include <string>
#include "vk_pipelines.h"

using namespace ar::vk;

int main() {
    PipeKey k{}; k.slots = 0x0123456789abcdefull; k.opts = 0x13; k.fmt = VK_FORMAT_R8G8B8A8_UNORM; k.blend = 1; k.depth = 1; k.flat = 2; k.samples = 4; k.cut = 1;
    const std::string line = pipeline_line(k);
    PipeKey back{}; CHECK(parse_pipeline_line(line, &back) && back == k);
    CHECK(line.find(std::to_string((int)VK_FORMAT_R8G8B8A8_UNORM)) != std::string::npos);
    k.cut = 0; CHECK(pipeline_line(k).size() < pipeline_line(back).size());        // the cut field is written only when 1
    CHECK(!parse_pipeline_line("1 2 3", &back));
    CHECK(!parse_pipeline_line(std::string(100, 'a'), &back));
    return 0;
}
