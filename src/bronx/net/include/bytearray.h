#pragma once


#include <memory>
#include <vector>
#include <string>
#include <sys/types.h>
#include <sys/socket.h>


namespace bronx{


class BxByteArray{
public:
    using ptr = std::shared_ptr<BxByteArray>;

    // ByteArray的存储节点(内存块)
    struct Node{
        // 构造指定大小的内存快
        Node(size_t s);

        // 无参构造
        Node();

        // 析构函数，负责释放内存
        ~Node();

        // 内存块地址
        char* ptr;
        // 下一个内存块地址
        Node* next;
        // 内存块大小
        size_t size;
    };

    // 使用指定大小的内存快构造ByteArray(默认4k)
    BxByteArray(size_t base_size = 4096);

    // 析构函数
    ~BxByteArray();

// 固定长度的写( position_ += sizeof(value) )
    // 写入固定长度int8_t类型的数据
    void writeFint8(int8_t value);

    // 写入固定长度uint8_t类型的数据
    void writeFuint8(uint8_t value);

    // 写入固定长度int16_t类型的数据
    void writeFint16(int16_t value);

    // 写入固定长度uint16_t类型的数据
    void writeFuint16(uint16_t value);

    // 写入固定长度int32_t类型的数据
    void writeFint32(int32_t value);

    // 写入固定长度uint32_t类型的数据
    void writeFuint32(uint32_t value);

    // 写入固定长度int64_t类型的数据
    void writeFint64(int64_t value);

    // 写入固定长度uint64_t类型的数据
    void writeFuint64(uint64_t value);

// 可变长度的写( position_ += 实际占用内存(1 ~ 5/10) )
    // varint: 每字节低 7 位存数据、最高位为"后续还有"标志,小数值只占 1~2 字节。
    // 有符号数先经 zigzag(把符号映射成无符号:0,-1,1,-2... → 0,1,2,3...)再 varint,
    // 避免负数因高位全 1 而占满字节。仅对 32/64 位提供(对 8/16 位压缩意义不大)。

    // 写入有符号Varint32类型的数据(zigzag 编码后按 varint 写)
    void writeInt32(int32_t value);

    // 写入有无符号Varint32类型的数据
    void writeUint32(uint32_t value);

    // 写入有符号Varint64类型的数据
    void writeInt64(int64_t value);

    // 写入有无符号Varint64类型的数据
    void writeUint64(uint64_t value);

// 其他类型的写
    void writeFloat(float value);

    void writeDouble(double value);

// 字符串写
    // 字符串长度的类型为uint16_t
    void writeStringF16(const std::string& value);

    // 字符串长度的类型为uint32_t
    void writeStringF32(const std::string& value);

    // 字符串长度的类型为uint64_t
    void writeStringF64(const std::string& value);

    // 字符串长度的类型为可变长（无符号Varint64）
    void writeVarStr(const std::string& value);

    // 写入字符串，无长度
    void writeRawStr(const std::string& value);


// 固定长度的读( position_ += sizeof(value) )
    // 读取固定长度int8_t类型的数据
    int8_t readFint8();

    // 读取固定长度uint8_t类型的数据
    uint8_t readFuint8();

    // 读取固定长度int16_t类型的数据
    int16_t readFint16();

    // 读取固定长度uint16_t类型的数据
    uint16_t readFuint16();

    // 读取固定长度int32_t类型的数据
    int32_t readFint32();

    // 读取固定长度uint32_t类型的数据
    uint32_t readFuint32();

    // 读取固定长度int64_t类型的数据
    int64_t readFint64();

    // 读取固定长度uint64_t类型的数据
    uint64_t readFuint64();

// 可变长度的读( position_ += 实际占用内存 )
    // 对一个字节（8位）和两个字节（16位）的压缩意义不大，所以只保留varint32和varint64

    // 读取有符号Varint32类型的数据
    int32_t readInt32();

    // 读取有无符号Varint32类型的数据
    uint32_t readUint32();

    // 读取有符号Varint64类型的数据
    int64_t readInt64();

    // 读取有无符号Varint64类型的数据
    uint64_t readUint64();

// 其他类型的读
    float readFloat();

    double readDouble();

// 字符串读
    // 字符串长度的类型为uint16_t
    std::string readStringF16();

    // 字符串长度的类型为uint32_t
    std::string readStringF32();

    // 字符串长度的类型为uint64_t
    std::string readStringF64();

    // 字符串长度的类型为可变长（无符号Varint64）
    std::string readVarStr();

// 内部操作
    // 清空ByteArray
    void clear();

// 辅助函数
    // 核心写:把 buf 的 size 字节追加到当前 position_ 处,按需 addCapacity 扩链表,
    // 跨 Node 边界自动续写;并前移 position_、按需抬高 size_。所有 write* 最终走这里。
    void write(const void* buf, size_t size);

    // 核心读:从 position_ 处读 size 字节到 buf,跨 Node 自动衔接并前移 position_;
    // 可读不足抛 out_of_range。所有 read* 最终走这里。
    void read(void* buf, size_t size);

    // 从指定 position 读 size 字节(const,不改 position_/cur_)。
    // 内部从 root_ 走到 position 所属 Node,故可任意位置随机读;toString 等依赖它。
    void read(void* buf, size_t size, size_t position) const;

// 数据成员相关
    // 返回ByteArray当前位置
    size_t getPosition() const { return position_; }
    // 设置读写游标到绝对位置 v,并同步把 cur_ 移到对应 Node;v 超出已分配容量抛异常。
    // 典型用途:写完数据后 setPosition(0) 切到读模式;或 socket 读入数据后前移游标。
    void setPosition(size_t v);

    // 返回内存块大小
    size_t getBaseSize() const { return baseSize_; }

    // 返回可读取数据的大小
    size_t getReadSize() const { return size_ - position_; }

    // 返回是否是小端
    bool isLittleEndian() const;

    // 设置是否是小端
    void setIsLittleEndian(bool val);

    // 返回数据长度
    size_t getSize() const { return size_; }

// 文件相关的读写方法
    // ByteArray数据写入文件
    //  name: 文件名
    bool writeToFile(const std::string& name) const;

    // 文件数据读取入ByteArray
    bool readFromFile(const std::string& name);

// 其他
    // 将ByteArray里面的数据[position_, size_)转成std::string
    std::string toString() const;

    // 将ByteArray里面的数据[position_, size_)转成16进制的std::string(格式:FF FF FF)
    std::string toHexString() const;

    // 把可读数据[position, position+len) 映射成 iovec 数组(不拷贝,每个 Node 一段),
    // 交给 writev/send 分散发送。返回实际可读长度。不改变 position_。
    // · buffers: 输出的 iovec 数组   · len: 期望长度   · position: 起始位置
    uint64_t peekReadIovec(std::vector<iovec>& buffers, uint64_t len, uint64_t position) const;
    uint64_t peekReadIovec(std::vector<iovec>& buffers, uint64_t len = ~0ull) const;

    // 先 addCapacity 备好 len 字节空闲空间,再把这段空闲区映射成 iovec 数组,
    // 交给 readv/recv 分散接收;接收后调用方需 setPosition 前移游标。不改变 position_。
    uint64_t peekWriteIovec(std::vector<iovec>& buffers, uint64_t len);

    char* getRoot() const { return root_->ptr; }
private:
// 内存相关操作
    // 扩容ByteArray
    void addCapacity(size_t size);

    // 获取当前可写入容量
    size_t getCapacity() const { return capacity_ - position_; }

private:
    // 内存块的大小（Node结构体容量的大小）
    size_t baseSize_;
    // 当前操作位置
    size_t position_;
    // 当前总容量
    size_t capacity_;
    // 当前数据大小
    size_t size_;
    // 字节序，默认为大端
    int8_t endian_;
    // 第一个内存块
    Node* root_;
    // 当前操作的内存快
    Node* cur_;
};

}