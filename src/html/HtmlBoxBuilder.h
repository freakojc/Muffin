#pragma once

#include "html/HtmlBox.h"

#include <memory>

namespace muffin::html {

class HtmlDocument;

class HtmlBoxBuilder {
public:
  HtmlBoxBuilder();
  ~HtmlBoxBuilder();

  std::unique_ptr<HtmlBox> build(const HtmlDocument& document);
  // Semantic import must preserve spaces separating adjacent inline elements.
  void setPreserveWhitespace(bool enabled) { preserveWhitespace_ = enabled; }

private:
  HtmlTag mapTag(void* node) const;
  void buildChildren(HtmlBox& parent, void* parentNode);
  std::unique_ptr<HtmlBox> buildNode(void* node);
  std::unique_ptr<HtmlBox> buildTextNode(void* textNode);
  bool shouldSkipTag(HtmlTag tag) const;
  void assignListMarkers(HtmlBox& box);
  static QString formatOrderedMarker(int index, HtmlListMarkerType type);
  static QString toRoman(int n);
  bool preserveWhitespace_ = false;
};

} // namespace muffin::html
