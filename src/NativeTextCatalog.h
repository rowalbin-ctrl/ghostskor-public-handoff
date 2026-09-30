#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Content lookup only. This object has no clock, screen position, HUD lane,
// mission name, visibility state or render queue.
namespace NativeTextCatalog {
using Map=std::unordered_map<std::string,std::string>;
using BindingFormatter=std::string(*)(const std::string&);
inline std::string Clean(const std::string &s) {
  std::string out;
  for(size_t i=0;i<s.size();++i) {
    if(s[i]=='^' && i+1<s.size() && s[i+1]>='0' && s[i+1]<='9') ++i;
    else if(s[i]!='\x1e' && s[i]!='\x1f') out+=s[i];
  }
  const auto start=out.find_first_not_of(" \t\r\n");
  return start==std::string::npos?std::string{}:out.substr(start,out.find_last_not_of(" \t\r\n")-start+1);
}
struct Part { std::string literal,token; };
struct BindingEdit {size_t start,end;std::string value;};
inline std::vector<Part> Parts(const std::string &s) {
  std::vector<Part> out;
  size_t literal=0;
  for(size_t i=0;i<s.size();) {
    size_t end=std::string::npos;
    // Includes binding tokens and older data's runtime-value placeholders.
    // Values come only from the actual rendered string; no synthetic clock.
    if(s.compare(i,2,"[{")==0) {const auto p=s.find("}]",i+2);if(p!=std::string::npos)end=p+2;}
    else if(s.compare(i,2,"&&")==0 && i+2<s.size() && s[i+2]>='1' && s[i+2]<='9')end=i+3;
    if(end==std::string::npos) {++i;continue;}
    out.push_back({s.substr(literal,i-literal),s.substr(i,end-i)});
    i=end;literal=end;
  }
  out.push_back({s.substr(literal),{}});
  return out;
}
struct Message {
  std::string key,english,korean;
  std::vector<Part> parts;
  size_t LiteralSize() const {size_t n=0;for(const auto &p:parts)n+=p.literal.size();return n;}
  std::string Translate(const std::string &rendered,BindingFormatter formatter=nullptr,
                        std::vector<BindingEdit> *bindingEdits=nullptr) const {
    const auto text=Clean(rendered);
    if(parts.size()==1) return text==parts[0].literal?korean:std::string{};
    Map values;
    size_t cursor=0;
    for(size_t i=0;i<parts.size();++i) {
      const auto &p=parts[i];
      if(text.compare(cursor,p.literal.size(),p.literal)) return {};
      cursor+=p.literal.size();
      if(p.token.empty()) return cursor==text.size()?Substitute(values):std::string{};
      const auto &tail=parts[i+1].literal;
      if(tail.empty() && i+2<parts.size()) return {}; // adjacent variables are ambiguous
      const size_t end=tail.empty()?text.size():text.find(tail,cursor);
      if(end==std::string::npos || end==cursor) return {};
      auto value=text.substr(cursor,end-cursor);
      if(value.find("[{")!=std::string::npos || value.find("&&")!=std::string::npos) return {};
      if(formatter && p.token.compare(0,3,"[{+")==0) {
        value=formatter(value);
        if(value.empty())return {};
        if(bindingEdits && value!=text.substr(cursor,end-cursor))
          bindingEdits->push_back({cursor,end,value});
      }
      const auto prior=values.find(p.token);
      if(prior!=values.end() && prior->second!=value) return {};
      values[p.token]=value;cursor=end;
    }
    return {};
  }
  std::string Substitute(const Map &values) const {
    std::string out;
    for(const auto &p:Parts(korean)) {
      out+=p.literal;
      if(p.token.empty())continue;
      const auto value=values.find(p.token);
      if(value==values.end())return {};
      out+=value->second;
    }
    return out;
  }
};
class Catalog {
  std::unordered_map<std::string,Message> messages;
  Map exact;
  std::unordered_set<std::string> ambiguous,dialogue;
  std::vector<std::string> templates;
  std::vector<Message> translatedBindingTemplates;
  BindingFormatter formatBinding=nullptr;
public:
  void Build(const Map &english,const Map &korean,BindingFormatter formatter=nullptr) {
    formatBinding=formatter;
    messages.clear();exact.clear();ambiguous.clear();dialogue.clear();templates.clear();translatedBindingTemplates.clear();
    for(const auto &entry:english) {
      const auto source=Clean(entry.second);
      if(entry.first.compare(0,9,"SUBTITLE_")==0 || entry.first.compare(0,13,"VIDSUBTITLES_")==0) {
        dialogue.insert(source);continue;
      }
      const auto kr=korean.find(entry.first);
      if(kr==korean.end() || kr->second.empty() || kr->second==entry.second || source.empty())continue;
      Message m{entry.first,entry.second,kr->second,Parts(source)};
      const bool dynamic=m.parts.size()>1;
      if(dynamic && kr->second.find("[{+")!=std::string::npos) {
        // Some native owners localize the sentence before resolving its
        // [{+action}] aliases. Match that whole translated template too; a
        // Korean sentence is not proof that its runtime key names are localized.
        translatedBindingTemplates.push_back({entry.first,kr->second,kr->second,Parts(Clean(kr->second))});
      }
      messages.emplace(entry.first,std::move(m));
      if(dynamic) {templates.push_back(entry.first);continue;}
      const auto old=exact.find(source);
      if(old!=exact.end() && messages.at(old->second).korean!=kr->second) ambiguous.insert(source);
      else exact[source]=entry.first;
    }
  }
  bool IsDialogue(const std::string &text) const { return dialogue.count(Clean(text))!=0; }
  std::string LocalizeRenderedBindings(const std::string &text,const std::string &sourceKey={},std::string *usedKey=nullptr) const {
    if(!formatBinding)return {};
    auto key=Clean(sourceKey);
    if(!key.empty() && key[0]=='@')key.erase(0,1);
    std::string answer,answerKey;bool conflict=false;
    for(const auto &message:translatedBindingTemplates) {
      std::vector<BindingEdit> edits;
      if(message.Translate(text,formatBinding,&edits).empty())continue;
      // Apply only captured binding edits to the native bytes, retaining
      // the sentence's actual color codes, control markers and edge padding.
      std::string plain;std::vector<size_t> positions;
      for(size_t i=0;i<text.size();++i) {
        if(text[i]=='^' && i+1<text.size() && text[i+1]>='0' && text[i+1]<='9')++i;
        else if(text[i]!='\x1e' && text[i]!='\x1f') {plain+=text[i];positions.push_back(i);}
      }
      const auto offset=plain.find_first_not_of(" \t\r\n");
      if(offset==std::string::npos)continue;
      std::string result=text;
      for(auto edit=edits.rbegin();edit!=edits.rend();++edit) {
        const auto begin=positions[offset+edit->start];
        const auto end=positions[offset+edit->end-1]+1;
        result.replace(begin,end-begin,edit->value);
      }
      if(message.key==key) {
        if(usedKey)*usedKey=message.key;
        return result==text?std::string{}:result;
      }
      if(answer.empty()) {answer=std::move(result);answerKey=message.key;}
      else if(answer!=result)conflict=true;
    }
    if(conflict || answer==text)return {};
    if(usedKey && !answer.empty())*usedKey=answerKey;
    return answer;
  }
  std::string OwnedTemplate(const std::string &text,const std::string &sourceKey) const {
    auto key=Clean(sourceKey);
    if(!key.empty() && key[0]=='@')key.erase(0,1);
    const auto found=messages.find(key);
    if(found==messages.end() || Clean(found->second.english)!=Clean(text))return {};
    std::unordered_set<std::string> before,after;
    for(const auto &p:found->second.parts)if(!p.token.empty())before.insert(p.token);
    for(const auto &p:Parts(found->second.korean))if(!p.token.empty())after.insert(p.token);
    // Only the actual owner's unchanged conversion tokens may reach the
    // engine's own formatter. Never infer an owner from a short prefix.
    return before==after?found->second.korean:std::string{};
  }
  std::string TranslateNativeLabel(const std::string &text,const std::string &labelKey,std::string *usedKey=nullptr) const {
    auto key=Clean(labelKey);
    if(!key.empty() && key[0]=='@')key.erase(0,1);
    const auto found=messages.find(key);
    if(found==messages.end())return {};
    auto message=found->second;
    // The native HUD preparer inserts its body at &&1, or appends it when
    // the label has no insertion marker. Restrict this to the actual label
    // key from that HUD element; never infer a label from a global prefix.
    if(message.parts.size()!=1 || Parts(message.korean).size()!=1)return {};
    message.english+="&&1";message.korean+="&&1";
    message.parts=Parts(Clean(message.english));
    auto result=message.Translate(text,formatBinding);
    if(!result.empty() && usedKey)*usedKey=key;
    return result;
  }
  std::string Translate(const std::string &text,const std::string &sourceKey={},std::string *usedKey=nullptr) const {
    auto key=Clean(sourceKey);
    if(!key.empty() && key[0]=='@')key.erase(0,1);
    const auto owned=messages.find(key);
    if(owned!=messages.end()) {
      auto result=owned->second.Translate(text,formatBinding);
      if(!result.empty()) {if(usedKey)*usedKey=owned->first;return result;}
    }
    const auto clean=Clean(text);
    const auto found=exact.find(clean);
    if(found!=exact.end() && !ambiguous.count(clean)) {
      if(usedKey)*usedKey=found->second;
      return messages.at(found->second).korean;
    }
    std::string answer,answerKey;
    size_t specificity=0;bool conflict=false;
    for(const auto &id:templates) {
      const auto &message=messages.at(id);
      auto result=message.Translate(text,formatBinding);
      if(result.empty())continue;
      const auto literalSize=message.LiteralSize();
      if(answer.empty() || literalSize>specificity) {
        specificity=literalSize;answer=std::move(result);answerKey=id;conflict=false;
      } else if(literalSize==specificity && answer!=result)conflict=true;
    }
    if(conflict)return {};
    if(usedKey && !answer.empty())*usedKey=answerKey;
    return answer;
  }
};
}
