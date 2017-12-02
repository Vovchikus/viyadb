#ifndef VIYA_CLUSTER_CONSUL_FEEDER_H_
#define VIYA_CLUSTER_CONSUL_FEEDER_H_

#include <unordered_map>
#include <vector>
#include <json.hpp>

namespace viya { namespace util { class Config; }}
namespace viya { namespace cluster { namespace consul { class Consul; }}}

namespace viya {
namespace cluster {
namespace feed {

class Notifier;

using json = nlohmann::json;

class Feeder {
  public:
    Feeder(const consul::Consul& consul, const util::Config& cluster_config,
           const std::unordered_map<std::string, util::Config>& table_configs);
    Feeder(const Feeder& other) = delete;
    ~Feeder();

  protected:
    void Start();
    void ProcessMicroBatch(const json& info);

  private:
    const consul::Consul& consul_;
    const util::Config& cluster_config_;
    const std::unordered_map<std::string, util::Config>& table_configs_;
    std::vector<Notifier*> notifiers_;
};

}}}

#endif // VIYA_CLUSTER_CONSUL_FEEDER_H_