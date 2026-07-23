#include "bytearray.h"
#include "log.h"
#include "_endian.h"
#include <cstring>
#include <cmath>
#include <iostream>
#include <iomanip>


static bronx::BxLogger::ptr g_logger = BRONX_LOG_NAME("system");

namespace bronx{


// 构造指定大小的内存快
BxByteArray::Node::Node(size_t s)
    : ptr(new char[s])
    , next(nullptr)
    , size(s){
}

// 无参构造
BxByteArray::Node::Node()
    : ptr(nullptr)
    , next(nullptr)
    , size(0){
}

// 析构函数，负责释放内存
BxByteArray::Node::~Node(){
    if(ptr){
        delete[] ptr;
    }
}


// 使用指定大小的内存快构造ByteArray(默认4k)
BxByteArray::BxByteArray(size_t base_size)
    : baseSize_(base_size)
    , position_(0)
    , capacity_(base_size)
    , size_(0)
    , endian_(BRONX_BIG_ENDIAN)
    , root_(new Node(base_size))
    , cur_(root_){
}

// 析构函数
BxByteArray::~BxByteArray(){
    Node* tmp = root_;
    while(tmp){
        cur_ = tmp;
        tmp = tmp->next;
        delete cur_;
    }
}

// 固定长度的写( position_ += sizeof(value) )
// 写入固定长度int8_t类型的数据
void BxByteArray::writeFint8(int8_t value){
    write(&value, sizeof(value));
}

// 写入固定长度uint8_t类型的数据
void BxByteArray::writeFuint8(uint8_t value){
    write(&value, sizeof(value));
}

// 写入固定长度int16_t类型的数据
void BxByteArray::writeFint16(int16_t value){
    // 大于一个字节（8bits）的数据需要先判断字节序
    // 只有固定长度需要考虑字节序
    if(endian_ != BRONX_BYTE_ORDER){
        value = byteswap(value);
    }
    write(&value, sizeof(value));
}

// 写入固定长度uint16_t类型的数据
void BxByteArray::writeFuint16(uint16_t value){
    if(endian_ != BRONX_BYTE_ORDER){
        value = byteswap(value);
    }
    write(&value, sizeof(value));  
}

// 写入固定长度int32_t类型的数据
void BxByteArray::writeFint32(int32_t value){
    if(endian_ != BRONX_BYTE_ORDER){
        value = byteswap(value);
    }
    write(&value, sizeof(value));
}

// 写入固定长度uint32_t类型的数据
void BxByteArray::writeFuint32(uint32_t value){
    if(endian_ != BRONX_BYTE_ORDER){
        value = byteswap(value);
    }
    write(&value, sizeof(value));
}

// 写入固定长度int64_t类型的数据
void BxByteArray::writeFint64(int64_t value){
    if(endian_ != BRONX_BYTE_ORDER){
        value = byteswap(value);
    }
    write(&value, sizeof(value));
}

// 写入固定长度uint64_t类型的数据
void BxByteArray::writeFuint64(uint64_t value){
    if(endian_ != BRONX_BYTE_ORDER){
        value = byteswap(value);
    }
    write(&value, sizeof(value));
}

// Zigzag压缩算法
static uint32_t EncodeZigzag32(const int32_t& v){
    // Zigzag: 负数→正奇数，正数(含0)→正偶数。
    // 标准做法用无符号位运算 (n<<1)^(n>>31)，全程无符号避免有符号溢出 UB：
    //  - v*2 对 INT32_MAX 溢出；-v 对 INT32_MIN 溢出。
    return ((uint32_t)v << 1) ^ (uint32_t)(v >> 31);
}

static uint64_t EncodeZigzag64(const int64_t& v){
    return ((uint64_t)v << 1) ^ (uint64_t)(v >> 63);
}

static int32_t DecodeZigzag32(const uint32_t& v){
    // (v & 1): 判断符号位，（正数为-0，负数为-1）
    // (v >> 1): 相当于 v / 2
    return (v >> 1) ^ -(v & 1);
}

static int64_t DecodeZigzag64(const uint64_t& v){
    return (v >> 1) ^ -(v & 1);
}


// 可变长度的写( position_ += 实际占用内存(1 ~ 5) )
// 写入有符号Varint32类型的数据
void BxByteArray::writeInt32(int32_t value){
    // 编码为无符号后转为调用无符号版本   
    writeUint32(EncodeZigzag32(value));
}

// 写入有无符号Varint32类型的数据
void BxByteArray::writeUint32(uint32_t value){
    uint8_t tmp[5];     // uint32最多5字节
    uint8_t i = 0;      // i记录数据的size（将tmp的size由5压缩到i）
    // 0x7f = 二进制1111111
    while(value > 0x7f){
        // 高位（低7bits以上）还有数据时：
        //  取出当前value的低7位存入tmp，再将value右移7位（移除低7位），继续判断高位

        // (value & 0x7F): 取出低7bits信息
        //  | 0x80: 低位向高位数第8位补一个1
        tmp[i++] = (value & 0x7f) | 0x80;
        // 移除低7位，继续判断高位
        value >>= 7;
    }
    tmp[i++] = value;
    write(tmp, i);
}

// 写入有符号Varint64类型的数据
void BxByteArray::writeInt64(int64_t value){
    writeUint64(EncodeZigzag64(value));
}

// 写入有无符号Varint64类型的数据
void BxByteArray::writeUint64(uint64_t value){
    uint8_t tmp[10];     // uint64最多10字节
    uint8_t i = 0;      // i记录数据的size（将tmp的size由10压缩到i）
    while(value > 0x7f){
        tmp[i++] = (value & 0x7f) | 0x80;
        value >>= 7;
    }
    tmp[i++] = value;
    write(tmp, i);
}

// 其他类型的写
void BxByteArray::writeFloat(float value){
    // float为固定32位，转换为整数处理，double同理
    uint32_t v;
    memcpy(&v, &value, sizeof(value));
    writeFuint32(v);
}

void BxByteArray::writeDouble(double value){
    // double固定64位
    uint64_t v;
    memcpy(&v, &value, sizeof(value));
    writeFuint64(v);  
}

// 字符串写
// 字符串长度的类型为固定uint16_t
void BxByteArray::writeStringF16(const std::string& value){
    writeFuint16(value.size());
    write(value.c_str(), value.size());
}

// 字符串长度的类型为固定uint32_t
void BxByteArray::writeStringF32(const std::string& value){
    writeFuint32(value.size());
    write(value.c_str(), value.size());
}

// 字符串长度的类型为固定uint64_t
void BxByteArray::writeStringF64(const std::string& value){
    writeFuint64(value.size());
    write(value.c_str(), value.size());
}

// 字符串长度的类型为可变长（无符号Varint64）
void BxByteArray::writeVarStr(const std::string& value){
    // 先对size进行压缩写入
    writeUint64(value.size());
    // 再写入字符串
    write(value.c_str(), value.size());
}

// 写入字符串，无长度
void BxByteArray::writeRawStr(const std::string& value){
    // 不需要写入长度，只写入字符串
    write(value.c_str(), value.size());
}


// 固定长度的读( position_ += sizeof(value) )
// 读取固定长度int8_t类型的数据
int8_t BxByteArray::readFint8(){
    int8_t v;
    read(&v, sizeof(v));
    return v;
}

// 读取固定长度uint8_t类型的数据
uint8_t BxByteArray::readFuint8(){
    uint8_t v;
    read(&v, sizeof(v));
    return v;
}

// 辅助宏，读取固定长度的整数
//  读取后需要处理字节序问题
#define XX(type) \
    type v; \
    read(&v, sizeof(v)); \
    if(endian_ != BRONX_BYTE_ORDER){ \
        return byteswap(v); \
    } \
    return v;

// 读取固定长度int16_t类型的数据
int16_t BxByteArray::readFint16(){
    XX(int16_t);
}

// 读取固定长度uint16_t类型的数据
uint16_t BxByteArray::readFuint16(){
    XX(uint16_t);
}

// 读取固定长度int32_t类型的数据
int32_t BxByteArray::readFint32(){
    XX(int32_t);
}

// 读取固定长度uint32_t类型的数据
uint32_t BxByteArray::readFuint32(){
    XX(uint32_t);
}

// 读取固定长度int64_t类型的数据
int64_t BxByteArray::readFint64(){
    XX(int64_t);
}

// 读取固定长度uint64_t类型的数据
uint64_t BxByteArray::readFuint64(){
    XX(uint64_t);
}

# undef XX

// 可变长度的读( position_ += 实际占用内存(1 ~ 5) )
// 读取有符号Varint32类型的数据
int32_t BxByteArray::readInt32(){
    // 调用无符号版本后再进行解码还原
    return DecodeZigzag32(readUint32());
}

// 读取有无符号Varint32类型的数据
uint32_t BxByteArray::readUint32(){
    uint32_t result = 0;
    for(int i = 0; i < 32; i += 7){
        // 背景：0 至 127（即 7位以内的整数）只需一个字节
        // 每次读8位，第8位为标志位，低7位为数据位（编码时确定）
        //  如果第8位为 0，表示当前读取的是最后一个字节；
        //  如果第8位为 1，表示后续还有字节需要读取

        // 一次读8位（一字节）
        uint8_t b = readFuint8();
        if(b < 0x80){
            // 第8位为0（b <= 0x7f）说明是末字节，已读完所有有效位。
            // 注意：必须用 < 0x80 而非 < 0x7f —— 当某个 7-bit 分组恰好等于
            // 0x7f(127) 时它也是末字节，用 < 0x7f 会误判为还有后续字节而读越界。
            result |= ((uint32_t)b) << i;
            break;
        } else {
            // 还有高位，需要先截出低位再左移
            result |= ((uint32_t)(b & 0x7f)) << i;
        }
    }
    return result;
}

// 读取有符号Varint64类型的数据
int64_t BxByteArray::readInt64(){
    return DecodeZigzag64(readUint64());
}

// 读取有无符号Varint64类型的数据
uint64_t BxByteArray::readUint64(){
    uint64_t result = 0;
    for(int i = 0; i < 64; i += 7){
        uint8_t b = readFuint8();
        if(b < 0x80){
            // 同 readUint32：末字节判定必须用 < 0x80，否则末组==0x7f 时读越界
            result |= ((uint64_t)b) << i;
            break;
        } else {
            result |= ((uint64_t)(b & 0x7f)) << i;
        }
    }
    return result;
}

// 其他类型的写
float BxByteArray::readFloat(){
    uint32_t v = readFuint32();
    float value;
    memcpy(&value, &v, sizeof(v));
    return value;
}

double BxByteArray::readDouble(){
    uint64_t v = readFuint64();
    double value;
    memcpy(&value, &v, sizeof(v));
    return value;
}

// 字符串写
// 字符串长度的类型为uint16_t
std::string BxByteArray::readStringF16(){
    uint16_t len = readFuint16();
    std::string buff;
    buff.resize(len);
    read(&buff[0], len);
    return buff;
}

// 字符串长度的类型为uint32_t
std::string BxByteArray::readStringF32(){
    uint32_t len = readFuint32();
    std::string buff;
    buff.resize(len);
    read(&buff[0], len);
    return buff;
}

// 字符串长度的类型为uint64_t
std::string BxByteArray::readStringF64(){
    uint64_t len = readFuint64();
    std::string buff;
    buff.resize(len);
    read(&buff[0], len);
    return buff;
}

// 字符串长度的类型为可变长（无符号Varint64）
std::string BxByteArray::readVarStr(){
    uint64_t len = readUint64();
    std::string buff;
    buff.resize(len);
    read(&buff[0], len);
    return buff; 
}

// 内部操作
// 清空ByteArray
void BxByteArray::clear(){
    // -清空ByteArray，重置数据成员状态
    position_ = 0;
    // size_ 必须一并清零，否则 clear 后 getSize()/getReadSize() 仍是旧值，
    // 复用该 BxByteArray 时会读到陈旧长度。
    size_ = 0;
    // -Node链表中只留一个root
    capacity_ = baseSize_;
    Node* tmp = root_->next;
    while(tmp){
        cur_ = tmp;
        tmp = tmp->next;
        delete cur_;
    }
    cur_ = root_;
    root_->next = nullptr;
}

// 辅助函数
// 将buf中size长度的数据写入ByteArray
void BxByteArray::write(const void* buf, size_t size){
    if(size == 0){
        return;
    }
    // 将容量增加到足够写完
    addCapacity(size);

    size_t npos = position_ % baseSize_;      // 写入点在当前Node中的位置
    size_t ncap = cur_->size - npos;           // 当前Node剩余容量
    size_t bpos = 0;        // buf中的数据写到哪了

    while(size > 0){
        if(ncap >= size){
            // 当前Node能写完
            memcpy(cur_->ptr + npos, (const char*)buf + bpos, size);
            if(cur_->size == (npos + size)){
                // 当前Node刚好写满，cur_指向next
                cur_ = cur_->next;
            }
            // 更新数据，本次写入了size字节的数据
            position_ += size;
            bpos += size;
            size = 0;
        } else {
            // 当前Node写不完
            memcpy(cur_->ptr + npos, (const char*)buf + bpos, ncap);
            // 更新数据，本次写入了ncap字节的数据
            position_ += ncap;
            bpos += ncap;
            size -= ncap;
            // 移到下一个Node
            cur_ = cur_->next;
            ncap = cur_->size;
            npos = 0;
        }
    }

    // 更新m_size
    if(position_ > size_){
        size_ = position_;
    }
}

// 向内存缓存指针buf处读取size长度的数据
void BxByteArray::read(void* buf, size_t size){
    // 读取的数据尺寸越界
    if(size > getReadSize()){
        throw std::out_of_range("Read: no enough len");
    }
    // size==0 是常规情形（如读空字符串），直接返回。
    // 否则下面会先解引用 cur_->size，而 cur_ 可能因上次正好读到 Node 边界而为
    // nullptr，导致空指针崩溃。
    if(size == 0){
        return;
    }

    // 下面逻辑和write差不多
    size_t npos = position_ % baseSize_;      // 写入点在当前Node中的位置
    size_t ncap = cur_->size - npos;           // 当前Node剩余容量
    size_t bpos = 0;        // buf中的数据读到哪了

    while(size > 0){
        if(ncap >= size){
            // 当前Node能读完
            memcpy((char*)buf + bpos, cur_->ptr + npos, size);
            if(cur_->size == (npos + size)){
                cur_ = cur_->next;
            }
            // 更新数据，本次读取了size字节的数据
            position_ += size;
            bpos += size;
            size = 0;
        } else {
            // 当前Node写不完
            memcpy((char*)buf + bpos, cur_->ptr + npos, ncap);
            // 更新数据，本次读取了ncap字节的数据
            position_ += ncap;
            bpos += ncap;
            size -= ncap;
            // 移到下一个Node
            cur_ = cur_->next;
            ncap = cur_->size;
            npos = 0;
        }
    }
}

// 指定读取开始的地方（不改变m_position与m_cur成员变量）
void BxByteArray::read(void* buf, size_t size, size_t position) const {
    if(size > (size_ - position)){
        throw std::out_of_range("Read: no enough len");
    }
    if(size == 0){
        return;   // 与 read(buf,size) 对称：空读直接返回，避免无谓的节点解引用
    }

    size_t npos = position % baseSize_;      // position 在所属 Node 内的偏移
    // 关键：从 root_ 走到 position 对应的 Node，而不是用绑定 position_ 的 cur_。
    // 否则当 position 与当前 position_ 不在同一 Node 时会读错节点甚至越界崩溃。
    Node* cur = root_;
    for(size_t skip = position / baseSize_; skip > 0 && cur; --skip){
        cur = cur->next;
    }
    size_t ncap = cur->size - npos;           // 该 Node 内从 npos 起的剩余容量
    size_t bpos = 0;        // buf中的数据读到哪了

    // BRONX_LOG_DEBUG(g_logger) << "BxByteArray::read buffer: " << std::string(cur->ptr, size);
    while(size > 0){
        if(ncap >= size){
            memcpy((char*)buf + bpos, cur->ptr + npos, size);
            if(cur->size == (npos + size)){
                cur = cur->next;
            }
            position += size;
            bpos += size;
            size = 0;
        } else {
            memcpy((char*)buf + bpos, cur->ptr + npos, ncap);
            position += ncap;
            bpos += ncap;
            size -= ncap;
            cur = cur->next;
            ncap = cur->size;
            npos = 0;
        }
    }
}

// 数据成员相关

// 设置ByteArray当前位置
void BxByteArray::setPosition(size_t v){
    if(v > capacity_){
        throw std::out_of_range("set_position out of range");
    }
    position_ = v;
    // 更新m_size
    if(position_ > size_){
        size_ = position_;
    }
    // 更新m_cur位置
    cur_ = root_;
    while(v > cur_->size){
        v -= cur_->size;
        cur_ = cur_->next;
    }
    if(v == cur_->size){
        cur_ = cur_->next;
    }
}

// 返回是否是小端
bool BxByteArray::isLittleEndian() const {
    return endian_ == BRONX_LITTLE_ENDIAN;
}

// 设置是否是小端
void BxByteArray::setIsLittleEndian(bool val){
    if(val){
        endian_ = BRONX_LITTLE_ENDIAN;
    } else {
        endian_ = BRONX_BIG_ENDIAN;
    }
}

// 文件相关的读写方法
bool BxByteArray::writeToFile(const std::string& name) const {
    std::ofstream ofs;
    // 以 截断 + 二进制 方式打开文件流（文件中的旧内容会被清空）
    ofs.open(name, std::ios::trunc | std::ios::binary);
    if(!ofs){
        BRONX_LOG_ERROR(g_logger) << "writeToFile name=" << name
                << " error, errno=" << errno << " errstr=" << strerror(errno);
        return false;
    }

    // 将可读内容 [position_, size_) 全部写入文件流。
    // 沿用 read(buf,size,position) 的规范游走:从 root_ 定位到 position_ 所在 Node,
    // 用 npos(节点内偏移)/ncap(节点内剩余)逐节点写。不信任成员 cur_(const 方法不该改
    // 对象状态),也不用"剩余长度算偏移"——那在 baseSize>1 且 position_ 未对齐节点边界时
    // len 会算错甚至变负、cur 与 pos 错位崩溃(原实现的潜在 bug,测试只测 position_=0 掩盖了)。
    size_t size = getReadSize();
    size_t position = position_;
    size_t npos = position % baseSize_;
    Node* cur = root_;
    for(size_t skip = position / baseSize_; skip > 0 && cur; --skip){
        cur = cur->next;
    }
    size_t ncap = cur ? (cur->size - npos) : 0;

    while(size > 0 && cur){
        size_t len = (ncap >= size) ? size : ncap;
        ofs.write(cur->ptr + npos, len);
        size -= len;
        if(ncap > len){
            // 当前节点还有剩余(只发生在最后一段),游标停在本节点
            npos += len;
            ncap -= len;
        } else {
            cur = cur->next;
            npos = 0;
            ncap = cur ? cur->size : 0;
        }
    }
    return true;
}

// 文件数据读取入ByteArray
bool BxByteArray::readFromFile(const std::string& name){
    std::ifstream ifs;
    ifs.open(name, std::ios::binary);
    if(!ifs){
        BRONX_LOG_ERROR(g_logger) << "readFromFile name=" << name
                << " error, errno=" << errno << " errstr=" << strerror(errno);
        return false;
    }

    // 使用智能指针管理Node链表
    std::shared_ptr<char> buff(new char[baseSize_], [](char* ptr){ delete[] ptr; });
    while(!ifs.eof()){
        // 一次读取一个Node的数据，读取结果存入buff
        ifs.read(buff.get(), baseSize_);
        // 将buff中的数据写入ByteArray
        write(buff.get(), ifs.gcount());
    }
    return true;
}

// 其他
// 将ByteArray里面的数据[position_, size_)转成std::string
std::string BxByteArray::toString() const {
    std::string str;
    str.resize(getReadSize());
    if(str.empty()){
        // 没有可读的数据。返回
        return str;
    }
    read(&str[0], str.size(), position_);
    return str;
}

// 将ByteArray里面的数据[position_, size_)转成16进制的std::string(格式:FF FF FF)
std::string BxByteArray::toHexString() const {
    // 获取字符串后再转为16进制
    std::string str = toString();
    std::stringstream ss;

    for(size_t i = 0; i < str.size(); ++i){
        if(i > 0 && i % 32 == 0){
            ss << std::endl;
        }
        // string中的每个字符都转换为16进制输出
        // std::setw(2) << std::setfill('0') << std::hex: 设置每个字符宽度为2字节，使用0填充，转换为16进制输出
        ss << std::setw(2) << std::setfill('0') << std::hex
           << (int)(uint8_t)str[i] << " ";
    }
    return ss.str();
}

// 获取可读取的缓存，保存为iovec数组
uint64_t BxByteArray::peekReadIovec(std::vector<iovec>& buffers, uint64_t len, uint64_t position) const {
    if(position >= size_){
        return 0;
    }
    len = std::min<uint64_t>(len, size_ - position);
    if(len == 0){
        return 0;
    }
    uint64_t size = len;    // 返回值，实际写入的数据size

    size_t npos = position % baseSize_;
    struct iovec iov;

    // 定位开始读取的位置
    size_t count = position / baseSize_;
    Node* cur = root_;
    while(count > 0){
        cur = cur->next;
        --count;
    }
    size_t ncap = cur->size - npos;

    // 每个iovec最多存储一个Node的数据
    while(len > 0){
        if(ncap >= len){
            iov.iov_base = cur->ptr + npos;
            iov.iov_len = len;
            len = 0;
        } else {
            iov.iov_base = cur->ptr + npos;
            iov.iov_len = ncap;
            len -= ncap;
            cur = cur->next;
            ncap = cur->size;
            npos = 0;
        }
        buffers.push_back(iov);
    }
    return size;
}

uint64_t BxByteArray::peekReadIovec(std::vector<iovec>& buffers, uint64_t len) const {
    len = std::min(len, getReadSize());
    if(len == 0){
        return 0;
    }
    uint64_t size = len;    // 返回值，实际写入的数据size

    size_t npos = position_ % baseSize_;
    size_t ncap = cur_->size - npos;
    struct iovec iov;
    Node* cur = cur_;

    // 每个iovec最多存储一个Node的数据
    while(len > 0){
        if(ncap >= len){
            iov.iov_base = cur->ptr + npos;
            iov.iov_len = len;
            len = 0;
        } else {
            iov.iov_base = cur->ptr + npos;
            iov.iov_len = ncap;
            len -= ncap;
            cur = cur->next;
            ncap = cur->size;
            npos = 0;
        }
        buffers.push_back(iov);
    }
    return size;
}

// 获取可写入的缓存，保存为iovec数组
uint64_t BxByteArray::peekWriteIovec(std::vector<iovec>& buffers, uint64_t len){
    if(len == 0){
        return 0;
    }
    addCapacity(len);
    uint64_t size = len;

    size_t npos = position_ % baseSize_;
    size_t ncap = cur_->size - npos;
    struct iovec iov;
    Node* cur = cur_;
    // BRONX_LOG_DEBUG(g_logger) << " peekWriteIovec: npos=" << npos 
    //     << " pointer_equal=" << (cur_->ptr - root_->ptr); 

    // 每个iovec最多存储一个Node的数据
    while(len > 0){
        if(ncap >= len){
            iov.iov_base = cur->ptr + npos;
            iov.iov_len = len;
            len = 0;
        } else {
            iov.iov_base = cur->ptr + npos;
            iov.iov_len = ncap;
            len -= ncap;
            cur = cur->next;
            ncap = cur->size;
            npos = 0;
        }
        buffers.push_back(iov);
    }
    return size;
}

// 扩容ByteArray
void BxByteArray::addCapacity(size_t size){
    if(size == 0){
        return;
    }
    size_t old_cap = getCapacity();
    if(old_cap >= size){
        return;
    }

    size -= old_cap;    // 还需要扩充的部分
    size_t count = ceil(1.0 * size / baseSize_);   // 需要增加的节点数
    Node* tmp = root_;
    while(tmp->next){
        // 移到到当前最后一个Node
        tmp = tmp->next;
    }

    Node* first = nullptr;      // first记录扩充的第一个节点
    for(size_t i = 0; i < count; ++i){
        // Node链表尾部新增一个节点
        tmp->next = new Node(baseSize_);
        if(!first){
            first = tmp->next;
        }
        tmp = tmp->next;
        capacity_ += baseSize_;
    }

    // 如果容量耗尽，cur_->next可能指向空，这里更新m_cur
    if(old_cap == 0){
        cur_ = first;
    }
}

}
