#pragma once

/*
 * 文件作用说明：
 * 极简 key=value 配置文件解析（无第三方依赖），供服务端从 config/ 预加载参数。
 * 约定：
 * - 每行一条：key = value 或 key=value；
 * - # 开头为注释；
 * - 键名不区分大小写（内部统一转小写存储）；
 * - 值首尾空白会被去掉；值中若需 # 请用引号（见实现）。
 */

#include <string>
#include <unordered_map>

namespace config_file {

// 从 path 读取配置；成功返回 true，失败时 err 含原因。
bool load(const std::string& path, std::unordered_map<std::string, std::string>* out, std::string* err);

// 取字符串，缺省为 defaultVal。
std::string getString(const std::unordered_map<std::string, std::string>& m, const std::string& key,
                      const std::string& defaultVal);

// 取整数；无法解析或缺省时返回 defaultVal。
int getInt(const std::unordered_map<std::string, std::string>& m, const std::string& key, int defaultVal);

// 取 long；无法解析或缺省时返回 defaultVal。
long getLong(const std::unordered_map<std::string, std::string>& m, const std::string& key, long defaultVal);

// 取布尔：1/true/yes/on 为真，0/false/no/off 为假；缺省或无法识别时返回 defaultVal。
bool getBool(const std::unordered_map<std::string, std::string>& m, const std::string& key, bool defaultVal);

}  // namespace config_file
