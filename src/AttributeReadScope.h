// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <functional>
namespace HfsPlus {
// Worker-local cancellation; no window/job references survive the scope.
class AttributeReadScope {
 std::function<void()> previous_;
public:
 explicit AttributeReadScope(std::function<void()>);
 ~AttributeReadScope();
 AttributeReadScope(const AttributeReadScope&)=delete;
 AttributeReadScope& operator=(const AttributeReadScope&)=delete;
};
void AttributeCheckpoint();
}
