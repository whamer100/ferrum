#include <iostream>
#include <nlohmann/json.hpp>
#include <cpr/cpr.h>

using json = nlohmann::json;

int main(int argc, char** argv) {
    std::cout << "Hello, World!" << std::endl;
    std::cout << std::setw(4) << json::meta() << std::endl;
    const std::string test_json_endpoint = "https://jsonplaceholder.typicode.com/todos/1";
    cpr::Response r = cpr::Get(
        cpr::Url{test_json_endpoint}
    );
    std::cout << r.status_code << '\n' << r.header["content-type"] << std::endl;
    const auto res = json::parse(r.text);
    std::cout << std::setw(4) << res << std::endl;
    return 0;
}
