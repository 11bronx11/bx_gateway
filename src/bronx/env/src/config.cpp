#include "config.h"
#include <list>
#include <sstream>
#include <vector>



namespace bronx{


// 按 name 在全局表里取配置项基类指针(读锁)。未命中返回 nullptr。
// LoadFromYaml 用它拿到 BxConfigVarBase::ptr 后再多态地 fromString。
BxConfigVarBase::ptr BxConfig::LookupBase(const std::string& name){
    RWMutexType::ReadLock lock(GetMutex());
    auto it = GetDatas().find(name);
    return (it == GetDatas().end()) ? nullptr : it->second;
}


// 把 YAML 树拍平成 <前缀键, 节点> 列表存进 output,方便逐项遍历。
static void ListAllMember(const std::string& prefix,
                          const YAML::Node& node,
                          std::list<std::pair<std::string, const YAML::Node>>& output){
    // 先转小写再校验:配置 key 大小写不敏感(统一以小写注册/查找)。
    // 原实现用原始大小写校验字符集(只允许小写),导致含大写字母的 key
    // 直接 throw → 整个 YAML 文件被静默丢弃。这里先小写化保证一致。
    std::string lower_prefix = prefix;
    std::transform(lower_prefix.begin(), lower_prefix.end(), lower_prefix.begin(), ::tolower);

    if(lower_prefix.find_first_not_of("1234567890._abcdefghijklmnopqrstuvwxyz")
        != std::string::npos){
        BRONX_LOG_ERROR(BRONX_LOG_ROOT()) << "Lookup name invalid: " << prefix;
        throw std::invalid_argument(prefix);
    }

    output.push_back(std::make_pair(lower_prefix, node));

    if(node.IsMap()){
        // 递归地处理map
        for(auto& it : node){
            ListAllMember(lower_prefix.empty() ? it.first.Scalar() : lower_prefix + "." + it.first.Scalar(),
                          it.second, output);
        }
    }
}


// 从 YAML 灌配置,热更新传导链就从这起(启动期 BxApplication::init 调)。
// 拍平成键值对后,逐项找已注册的同名配置项 fromString 灌进去,值变了会顺带回放监听器
// (比如日志层据 "logs" 重建 appender)。没注册过的键直接跳过。
void BxConfig::LoadFromYaml(const YAML::Node& root){
    // 将Yaml的树状结构拍平为list（方便遍历）
    // <name, yaml_node>
    std::list<std::pair<std::string, const YAML::Node>> all_nodes;
    ListAllMember("", root, all_nodes);

    // 遍历Yaml，逐个解析配置项
    for(auto& item : all_nodes){
        std::string key = item.first;
        if(key.empty()){
            continue;
        }

        // 将键转换为全小写
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);

        BxConfigVarBase::ptr var = LookupBase(key);
        // 从字符串获取配置项var的值
        if(var){
            if(item.second.IsScalar()){
                var->fromString(item.second.Scalar());
            } else {
                std::stringstream ss;
                ss << item.second;
                // ss.str()为Yaml格式的字符串，交给fromString来解析
                var->fromString(ss.str());
            }
        }
    }
}


// 遍历所有已注册配置项并对每个执行 cb。
// 先在读锁内快照到本地 vector 再出锁回调,既缩短持锁时间,也允许 cb 内部
// 安全地读其他配置而不死锁。常用于把全部配置 dump 成 YAML。
void BxConfig::Visit(std::function<void(BxConfigVarBase::ptr)> cb){
    std::vector<BxConfigVarBase::ptr> vars;
    {
        RWMutexType::ReadLock lock(GetMutex());
        auto& mp = GetDatas();
        vars.reserve(mp.size());
        for(auto& i : mp){
            vars.push_back(i.second);
        }
    }

    for(auto& var : vars){
        cb(var);
    }
}





}
