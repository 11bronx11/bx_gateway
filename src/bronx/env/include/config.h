#pragma once

#include <memory>
#include <string>
#include <sstream>
#include <map>
#include <vector>
#include <list>
#include <set>
#include <map>
#include <unordered_set>
#include <unordered_map>
#include <functional>
#include <atomic>
#include <boost/lexical_cast.hpp>
#include <yaml-cpp/yaml.h>
#include "log.h"
#include "sync.h"


namespace bronx{


// 配置层:YAML 文本 <-> 内存里的强类型 BxConfigVar<T>,值变了用监听器通知别人
// (比如日志层据此重建 appender)。启动期用,不在协程调度主链里。
// 用法:Lookup 登记配置项(多在静态初始化期),启动时 LoadFromYaml 把磁盘值灌进去。


// F -> T 的转换仿函数,配置序列化就靠它。裹了一层 boost::lexical_cast,可特化。
// 下面给 vector/list/set/map 做了偏特化,借 yaml-cpp 递归转。map 的 key 只支持 string。
// 泛化版直接用于简单类型,也被特化版递归调用。
template<typename F, typename T>
class LexicalCast{
public:
    T operator()(const F& val){
        return boost::lexical_cast<T>(val);
    }
};


// vector -> Yaml格式字符串
template<typename T>
class LexicalCast<std::vector<T>, std::string>{
public:
    std::string operator()(const std::vector<T> vec){
        YAML::Node node(YAML::NodeType::Sequence);
        for(auto& i : vec){
            // 元素也递归走一遍 LexicalCast
            node.push_back(YAML::Load(LexicalCast<T, std::string>()(i)));
        }
        std::stringstream ss;
        ss << node;
        return ss.str();
    }
};

// Yaml格式字符串 -> vector
template<typename T>
class LexicalCast<std::string, std::vector<T>>{
public:
    std::vector<T> operator()(const std::string str){
        YAML::Node nodes = YAML::Load(str);
        std::vector<T> vec;
        std::stringstream ss;
        for(const auto& node : nodes){
            ss.str("");
            ss << node;
            vec.push_back(LexicalCast<std::string, T>()(ss.str()));
        }
        return vec;
    }
};

// list、set、unordered_set：实现类似vector，NodeType都为Sequence
// map、unordered_map：NodeType为Map，只支持了key为std::string

// list
// list -> Yaml格式字符串
template<typename T>
class LexicalCast<std::list<T>, std::string>{
public:
    std::string operator()(const std::list<T> lst){
        YAML::Node node(YAML::NodeType::Sequence);
        for(auto& i : lst){
            node.push_back(YAML::Load(LexicalCast<T, std::string>()(i)));
        }
        std::stringstream ss;
        ss << node;
        return ss.str();
    }
};

// Yaml格式字符串 -> list
template<typename T>
class LexicalCast<std::string, std::list<T>>{
public:
    std::list<T> operator()(const std::string str){
        YAML::Node nodes = YAML::Load(str);
        std::list<T> lst;
        std::stringstream ss;
        for(const auto& node : nodes){
            ss.str("");
            ss << node;
            lst.push_back(LexicalCast<std::string, T>()(ss.str()));
        }
        return lst;
    }
};

// set
// set -> Yaml格式字符串
template<typename T>
class LexicalCast<std::set<T>, std::string>{
public:
    std::string operator()(const std::set<T> st){
        YAML::Node node(YAML::NodeType::Sequence);
        for(auto& i : st){
            node.push_back(YAML::Load(LexicalCast<T, std::string>()(i)));
        }
        std::stringstream ss;
        ss << node;
        return ss.str();
    }
};

// Yaml格式字符串 -> set
template<typename T>
class LexicalCast<std::string, std::set<T>>{
public:
    std::set<T> operator()(const std::string str){
        YAML::Node nodes = YAML::Load(str);
        std::set<T> st;
        std::stringstream ss;
        for(const auto& node : nodes){
            ss.str("");
            ss << node;
            st.insert(LexicalCast<std::string, T>()(ss.str()));
        }
        return st;
    }
};

// unordered_set
// unordered_set -> Yaml格式字符串
template<typename T>
class LexicalCast<std::unordered_set<T>, std::string>{
public:
    std::string operator()(const std::unordered_set<T> ust){
        YAML::Node node(YAML::NodeType::Sequence);
        for(auto& i : ust){
            node.push_back(YAML::Load(LexicalCast<T, std::string>()(i)));
        }
        std::stringstream ss;
        ss << node;
        return ss.str();
    }
};

// Yaml格式字符串 -> unordered_set
template<typename T>
class LexicalCast<std::string, std::unordered_set<T>>{
public:
    std::unordered_set<T> operator()(const std::string str){
        YAML::Node nodes = YAML::Load(str);
        std::unordered_set<T> ust;
        std::stringstream ss;
        for(const auto& node : nodes){
            ss.str("");
            ss << node;
            ust.insert(LexicalCast<std::string, T>()(ss.str()));
        }
        return ust;
    }
};

// map，只支持key类型为std::string
// map -> Yaml格式字符串
template<typename T>
class LexicalCast<std::map<std::string, T>, std::string>{
public:
    std::string operator()(const std::map<std::string, T> mp){
        YAML::Node node(YAML::NodeType::Map);
        for(auto& i : mp){
            node[i.first] = YAML::Load(LexicalCast<T, std::string>()(i.second));
        }
        std::stringstream ss;
        ss << node;
        return ss.str();
    }
};

// Yaml格式字符串 -> map
template<typename T>
class LexicalCast<std::string, std::map<std::string, T>>{
public:
    std::map<std::string, T> operator()(const std::string str){
        YAML::Node nodes = YAML::Load(str);
        std::map<std::string, T> mp;
        std::stringstream ss;
        for(const auto& node : nodes){
            ss.str("");
            ss << node.second;
            mp.insert(std::make_pair(node.first.Scalar(),
                                     LexicalCast<std::string, T>()(ss.str())));
        }
        return mp;
    }
};

// unordered_map，只支持key类型为std::string
// unordered_map -> Yaml格式字符串
template<typename T>
class LexicalCast<std::unordered_map<std::string, T>, std::string>{
public:
    std::string operator()(const std::unordered_map<std::string, T> mp){
        YAML::Node node(YAML::NodeType::Map);
        for(auto& i : mp){
            node[i.first] = YAML::Load(LexicalCast<T, std::string>()(i.second));
        }
        std::stringstream ss;
        ss << node;
        return ss.str();
    }
};

// Yaml格式字符串 -> unordered_map
template<typename T>
class LexicalCast<std::string, std::unordered_map<std::string, T>>{
public:
    std::unordered_map<std::string, T> operator()(const std::string str){
        YAML::Node nodes = YAML::Load(str);
        std::unordered_map<std::string, T> ump;
        std::stringstream ss;
        for(const auto& node : nodes){
            ss.str("");
            ss << node.second;
            ump.insert(std::make_pair(node.first.Scalar(),
                                     LexicalCast<std::string, T>()(ss.str())));
        }
        return ump;
    }
};


// 配置项的类型擦除基类。模板化的 BxConfigVar<T> 类型各不相同,抽出这个非模板
// 基类,BxConfig 才能拿一个 map 统一装下,再靠虚函数 toString/fromString 多态转。
class BxConfigVarBase{
public:
    using ptr = std::shared_ptr<BxConfigVarBase>;

    BxConfigVarBase(const std::string& name, const std::string& description = "")
        :name_(name)
        ,description_(description){
    }

    virtual ~BxConfigVarBase(){}

    std::string getName() const { return name_; }

    std::string getDescription() const { return description_; }

    virtual std::string getTypename() const = 0;

    // 将配置参数的值转换为字符串
    virtual std::string toString() = 0;

    // 从字符串中获取配置参数初始值
    virtual bool fromString(const std::string& val) = 0;

private:
    // 配置参数的名称
    std::string name_;
    // 配置参数的描述
    std::string description_;
};


// 单个强类型配置项,配置层的核心。装某个键的值 T,线程安全读写、YAML <-> T 互转,
// 外加值变更监听器 —— 热更新能传导出去就靠它。值真变了才在锁外回放监听器
// (log.cpp 就在 "logs" 项挂了回调,据此重建 appender)。
// 回调放锁外,是为了让回调里能安全地再读配置/增删监听器,不自死锁。
// 两个模板参数是转换仿函数:FromStr 把 string 转成 T,ToStr 反过来。
template<typename T, typename FromStr = LexicalCast<std::string, T>
                   , typename ToStr = LexicalCast<T, std::string>>
class BxConfigVar: public BxConfigVarBase{
public:
    using ptr = std::shared_ptr<BxConfigVar>;
    using on_change_cb = std::function<void(const T& oldVal, const T& newVal)>;
    using RWMutexType = BxRwMutex;

    BxConfigVar(const std::string& name
            , const T& val
            , const std::string& description = "")
        : BxConfigVarBase(name, description)
        , val_(val){
    }

    // 调用ToStr()将配置值转换为Yaml字符串返回
    // 在锁内读取 val_ 再交给 ToStr 序列化;转换异常吞掉并记日志,返回空串。
    std::string toString() override {
        try{
            RWMutexType::ReadLock lock(mutex_);
            // return boost::lexical_cast<std::string>(val_);
            return ToStr()(val_);
        } catch(std::exception& e) {
            BRONX_LOG_ERROR(BRONX_LOG_ROOT()) << "BxConfigVar::toString exception: "
                << e.what() << " convert: " << typeid(val_).name() << " to string";
        }
        return "";
    }

    // 调用FromStr()将Yaml字符串解析为配置值
    // LoadFromYaml 灌值的落点:把 YAML 串经 FromStr 转成 T 后转交 setValue
    // (因而会触发监听器)。转换失败返回 false 并记日志,不改变原值。
    bool fromString(const std::string& val) override {
        try{
            // setValue 中进行了加锁操作，这里不需要额外加锁
            // val_ = boost::lexical_cast<T>(val);
            setValue(FromStr()(val));
            return true;
        } catch(std::exception& e) {
            BRONX_LOG_ERROR(BRONX_LOG_ROOT()) << "ConfiVar::fromString exception: "
                << e.what() << " convert string to " << typeid(val_).name(); 
        }
        return false;
    }

    // 获取配置项的值
    const T getValue() const {
        RWMutexType::ReadLock lock(mutex_);
        return val_;
    }
    // 设置配置项的值
    // 配置热更新的传导枢纽:值确实变化(operator!= 判定)时,在写锁内换值并
    // 快照监听器表,出锁后逐个回放 (oldVal,newVal)。值未变则直接返回、不触发
    // 回调。回调置于锁外,避免回调里再操作本配置项造成自死锁。
    void setValue(const T& val){
        std::map<uint64_t, on_change_cb> cbs;
        T oldVal = val;
        {
            RWMutexType::WriteLock lock(mutex_);
            if(val == val_){
                return;
            }
            oldVal = val_;
            val_ = val;
            cbs = cbs_;
        }

        // 回调在锁外执行，允许回调中安全地增删监听器或读取配置。
        for(auto& cb : cbs){
            cb.second(oldVal, val);
        }
    }

    std::string getTypename() const override { return typeid(T).name(); }

    // 添加回调函数，返回该回调函数的唯一id
    // 监听器注册入口:返回的 id 可用于 delListener 摘除。典型调用方是各子系统
    // 在启动期挂回调(如日志系统监听 "logs" 项),以便配置热更新时被通知重建。
    uint64_t addListener(on_change_cb cb){
        // 自动创建回调函数对应的key
        static std::atomic<uint64_t> s_cbfun_id{0};

        RWMutexType::WriteLock lock(mutex_);
        uint64_t id = s_cbfun_id.fetch_add(1, std::memory_order_relaxed) + 1;
        cbs_[id] = cb;
        return id;
    };

    void delListener(uint64_t key){
        RWMutexType::WriteLock lock(mutex_);
        cbs_.erase(key);
    };

    on_change_cb getListener(uint64_t key){
        RWMutexType::ReadLock lock(mutex_);
        auto it = cbs_.find(key);
        return it == cbs_.end() ? nullptr : it->second;
    };

    void clearListener(){
        RWMutexType::WriteLock lock(mutex_);
        cbs_.clear();
    };


private:
    // 配置项的值
    T val_;
    // 管理回调函数的map
    std::map<uint64_t, on_change_cb> cbs_;
    // 读写锁
    mutable RWMutexType mutex_;
};


// 全局配置注册表,一张静态表装下全进程所有配置项,没有实例全是静态成员。
// 表和锁都用 GetDatas/GetMutex 里的静态局部变量懒初始化,躲开静态初始化顺序坑
// (Effective C++ 条款 4)。各模块启动期调 Lookup 登记,LoadFromYaml 批量灌值。
// 几个坑:key 大小写不敏感(内部统一转小写);Lookup 时类型对不上返回 nullptr 不抛。
// 读多写少,用读写锁。
class BxConfig{
public:
    using ConfigVarMap = std::unordered_map<std::string, BxConfigVarBase::ptr>;
    using RWMutexType = BxRwMutex;

    // 根据name在容器中查找配置项
    // 只读版本:命中则 dynamic_pointer_cast 成 BxConfigVar<T>,类型不符或未命中
    // 都返回 nullptr。不创建新项。
    template<typename T>
    static typename BxConfigVar<T>::ptr Lookup(const std::string& name){
        // 这个Lookup版本只进行读操作，所以上读锁
        RWMutexType::ReadLock lock(GetMutex());

        auto it = GetDatas().find(name);
        if(it != GetDatas().end()){
            return std::dynamic_pointer_cast<BxConfigVar<T>>(it->second);
        }
        return nullptr;
    }
    
    // 在容器中查找配置项，如果找不到就尝试新增配置项
    // 配置项注册的主入口(各模块静态初始化期调用)。先读锁查、命中即返回;未命中
    // 则校验 name 字符集(非法字符抛 invalid_argument),再上写锁二次确认后插入,
    // 避免多线程重复创建。返回的 BxConfigVar<T> 此时只持有 default_value,真实值
    // 要等启动期 LoadFromYaml 灌入。
    template<typename T>
    static typename BxConfigVar<T>::ptr Lookup(const std::string& name,
            const T& default_value, const std::string& description = ""){
        auto& mp = GetDatas();
        // 先上一个读锁，进行查询操作
        {
            RWMutexType::ReadLock lock(GetMutex());
            auto it = mp.find(name);
            if(it != GetDatas().end()){
                return BxConfig::pointer_cast<T>(name, it->second);
            }
        }

        // 检查name中是否所有字符都合法
        if(name.find_first_not_of("1234567890._abcdefghijklmnopqrstuvwxyz")
            != std::string::npos){
            // name中存在非法字符，写入错误log并抛出invalid_argument异常
            // BRONX_LOG_ERROR(BRONX_LOG_ROOT()) << "Lookup name invalid: " << name;
            throw std::invalid_argument(name);
        }

        // 如果查询失败，就尝试在容器中新增一个配置参数项
        // 上写锁，进行插入操作
        RWMutexType::WriteLock lock(GetMutex());
        // 再次检查 name 是否存在，避免多线程重复插入的问题
        auto it = mp.find(name);
        if(it != mp.end()){
            return BxConfig::pointer_cast<T>(name, it->second);
        }
        typename BxConfigVar<T>::ptr newConfigVar = std::make_shared<BxConfigVar<T>>(name, default_value, description);
        mp[name] = newConfigVar;
        // BRONX_LOG_DEBUG(BRONX_LOG_ROOT()) << "Add config name: " << name << ", type: "<< typeid(T).name();
        // BRONX_LOG_DEBUG(BRONX_LOG_ROOT()) << " -Add config: " << name;
        return newConfigVar;
    }

    // 按 name 查,返回基类指针
    static BxConfigVarBase::ptr LookupBase(const std::string& name);

    // 从Yaml中加载配置
    // 启动期(BxApplication::init)的灌值入口:把 YAML 树拍平为 <小写键, 节点>,
    // 对每个已注册的同名配置项调用 fromString,从而触发其监听器。未注册的键被
    // 忽略。是配置 -> 各子系统热更新传导链的起点。
    static void LoadFromYaml(const YAML::Node& root);

    // Visit接收一个回调函数cb，对Config管理的所有配置项应用该回调函数
    // 先在读锁内把所有项快照到本地 vector,再出锁逐个回调(便于 cb 内部安全
    // 读配置而不长时间持锁)。常用于把全部配置 dump 成 YAML。
    static void Visit(std::function<void(BxConfigVarBase::ptr)> cb);

private:
    // 封装 dynamic_pointer_cast 和类型检查逻辑操作
    template<typename T>
    static typename BxConfigVar<T>::ptr pointer_cast(const std::string& name, BxConfigVarBase::ptr val){
        typename BxConfigVar<T>::ptr tmp = std::dynamic_pointer_cast<BxConfigVar<T>>(val);;
        if(tmp){
            //BRONX_LOG_INFO(BRONX_LOG_ROOT()) << "Lookup name=" << name << "exists.";
            return tmp;
        } else {
            //BRONX_LOG_ERROR(BRONX_LOG_ROOT()) << "Lookup name=" << name << ", but type not <"
                //<< typeid(T).name() << ">, real type=<" << val->getTypename() << ">";
        }
        return nullptr;
    }

    // 用静态局部变量兜住,躲开静态初始化顺序问题
    static ConfigVarMap& GetDatas(){
        static ConfigVarMap s_datas;
        return s_datas;
    }

    // 因为读写锁应用的对象为静态变量，所以也需要使用静态成员函数获得读写锁
    static RWMutexType& GetMutex(){
        static RWMutexType s_mutex;
        return s_mutex;
    }

};


}
