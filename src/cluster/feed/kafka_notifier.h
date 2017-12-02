#ifndef VIYA_CLUSTER_CONSUL_KAFKA_NOTIFIER_H_
#define VIYA_CLUSTER_CONSUL_KAFKA_NOTIFIER_H_

#include <memory>
#include <unordered_map>
#include "cluster/feed/notifier.h"

namespace cppkafka { class Consumer; class Configuration; }
namespace viya { namespace util { class Always; }}

namespace viya {
namespace cluster {
namespace feed {

class KafkaNotifier: public Notifier {
  public:
    KafkaNotifier(const util::Config& config);

    void Listen(std::function<void(const json& info)> callback);
    std::vector<json> GetAllMessages();
    json GetLastMessage();

  protected:
    cppkafka::Configuration CreateConsumerConfig(const std::string& group_id);
    std::unordered_map<uint32_t, int64_t> GetLatestOffsets(cppkafka::Consumer& consumer);

  private:
    const std::string brokers_;
    const std::string topic_;
    std::unique_ptr<cppkafka::Consumer> consumer_;
    std::unique_ptr<util::Always> always_;
};

}}}

#endif // VIYA_CLUSTER_CONSUL_KAFKA_NOTIFIER_H_