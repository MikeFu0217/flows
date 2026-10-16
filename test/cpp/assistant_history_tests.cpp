#include "AssistantHistory.hpp"
#include <iostream>
#include <stdexcept>

using namespace juce;
using namespace flows::app;
namespace {
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F> void rejects(F&& action,const char* message){try{action();}catch(const std::exception&){return;}throw std::runtime_error(message);}
struct Workspace {
    File directory=File::getSpecialLocation(File::tempDirectory).getNonexistentChildFile("flows-history-tests",{},false);
    Workspace(){check(directory.createDirectory().wasOk(),"Could not create isolated workspace");}
    ~Workspace(){directory.deleteRecursively();}
    File history()const{return directory.getChildFile(".assistant-history.json");}
};
var selected(AssistantHistory& history){return get(history.view(),"conversation");}
var turn(AssistantHistory& history,int index){auto messages=get(selected(history),"messages");return messages.getArray()->getReference(index);}
void persistence_and_lifecycle(){
    Workspace workspace;String id;
    {
        AssistantHistory history;history.setWorkspace(workspace.directory);check(history.error().isEmpty(),"Empty history failed to load");
        id=history.create();check(history.activeId()==id,"Create did not select conversation");
        check(history.begin(id,"request-one","Explain this filter","bass.flows",7)==id,"Send targeted wrong conversation");
        rejects([&]{history.archive(id);},"Archived live request");rejects([&]{history.remove(id);},"Deleted live request");
        history.finish(id,"request-one","done","# Filter\n\nA **lowpass** keeps lower frequencies.","proposed source");
        check(str(selected(history),"title")=="Explain this filter","First prompt did not name conversation");
        check(history.proposalAvailable(id,"request-one","proposed source"),"Saved proposal lookup failed");
        history.rename(id,"Lowpass experiments");history.archive(id);
        check(!history.proposalAvailable(id,"request-one","proposed source"),"Archived proposal remained actionable");check(static_cast<bool>(get(selected(history),"archived")),"Archive did not persist");
        rejects([&]{history.begin(id,"archived-send","hello","bass.flows",7);},"Sent to archive");
        history.restore(id);history.remove(id);check(static_cast<bool>(get(selected(history),"deleted")),"Delete did not move to Trash");
        rejects([&]{history.begin(id,"trash-send","hello","bass.flows",7);},"Sent to Trash");history.restore(id);
    }
    {
        AssistantHistory history;history.setWorkspace(workspace.directory);check(history.activeId()==id,"Restart lost selection");
        auto conversation=selected(history);check(str(conversation,"title")=="Lowpass experiments","Restart lost renamed title");
        check(static_cast<int>(get(conversation,"messageCount"))==1,"Restart lost message");
        auto message=turn(history,0);check(str(message,"text").contains("**lowpass**")&&str(message,"documentId")=="bass.flows"&&str(message,"proposedSource")=="proposed source","Restart lost full turn fields");
        check(get(message,"proposalAvailable").isVoid(),"Persisted an actionable proposal token");
        rejects([&]{history.purge(id);},"Purged a conversation outside Trash");history.remove(id);history.purge(id);
        check(get(history.view(),"conversations").getArray()->isEmpty(),"Permanent delete left conversation");
    }
}
void routed_completion_and_context(){
    Workspace workspace;AssistantHistory first,second;first.setWorkspace(workspace.directory);second.setWorkspace(workspace.directory);
    auto a=first.create("Instrument A"),b=second.create("Instrument B");
    first.begin(a,"first-running","Show A","a.flows",10);
    rejects([&]{first.begin(b,"first-running","Duplicate ID","b.flows",12);},"Accepted a reused request ID");
    first.select(a);check(str(turn(first,0),"status")=="working","Rejected duplicate ID interrupted the original request");
    first.select(b);
    second.begin(b,"second-running","Show B","b.flows",12);
    first.finish(a,"first-running","done","A response");second.finish(b,"second-running","done","B response");
    first.select(a);check(str(turn(first,0),"text")=="A response","Completion followed selected conversation instead of request");
    auto summaries=get(first.view(),"conversations");check(summaries.getArray()->size()==2,"Multiple instances overwrote each other's conversations");
    first.begin(a,"failed-turn","failed question","a.flows",13);first.finish(a,"failed-turn","failed","Provider failed");
    first.begin(a,"cancelled-turn","cancelled question","a.flows",14);first.finish(a,"cancelled-turn","cancelled","Cancelled");
    first.begin(a,"second-good","Continue A","c.flows",15);first.finish(a,"second-good","done","Second A response");
    auto context=first.context(a);check(context.size()==4,"Context included incomplete turns or missed completed turns");
    check(str(context[0],"role")=="user"&&str(context[1],"role")=="assistant"&&str(context[2],"role")=="user"&&str(context[3],"role")=="assistant","History roles are unpaired");
    check(str(context[0],"content").contains("a.flows")&&str(context[2],"content").contains("c.flows"),"Historical instrument context omitted");
    for(int i=0;i<18;++i){auto request="limit-"+String(i);first.begin(a,request,"question "+String(i),"a.flows",20+i);first.finish(a,request,"done","answer "+String(i));}
    context=first.context(a);check(context.size()==32&&str(context[0],"content").startsWith("question 2"),"Context did not keep most recent 16 successful turns");
}
void interrupted_recovery_and_corruption(){
    Workspace workspace;String id;
    {
        AssistantHistory live;live.setWorkspace(workspace.directory);id=live.create();live.begin(id,"unfinished","Continue later","a.flows",2);
        AssistantHistory another;another.setWorkspace(workspace.directory);another.select(id);check(str(turn(another,0),"status")=="working","A live owner's request was interrupted by another instance");
    }
    {
        AssistantHistory recovered;recovered.setWorkspace(workspace.directory);recovered.select(id);
        check(str(turn(recovered,0),"status")=="interrupted","Restart did not recover abandoned pending request");
        recovered.begin(id,"retry","Retry now","a.flows",2);recovered.finish(id,"retry","done","Retry completed");
    }
    const String corrupted="{ broken history";check(workspace.history().replaceWithText(corrupted),"Could not create corrupted fixture");
    AssistantHistory blocked;blocked.setWorkspace(workspace.directory);check(blocked.error().isNotEmpty(),"Corrupted file was silently accepted");
    rejects([&]{blocked.create();},"Overwrote corrupted history");check(workspace.history().loadFileAsString()==corrupted,"Corruption handling destroyed original file");
    check(str(blocked.view(),"historyError").isNotEmpty(),"Corruption did not return actionable UI error");
}
void terminal_save_failure_recovery(){
    Workspace workspace;AssistantHistory history;history.setWorkspace(workspace.directory);const auto id=history.create();
    history.begin(id,"write-failed","Explain A","a.flows",1);
    const auto backup=workspace.directory.getChildFile("preserved-history.json");
    check(workspace.history().moveFileTo(backup)&&workspace.history().createDirectory().wasOk(),"Could not inject terminal history failure");
    rejects([&]{history.finish(id,"write-failed","done","Answer");},"Terminal save unexpectedly succeeded through an obstruction");
    rejects([&]{history.finish(id,"write-failed","failed","Save failed");},"Failure save unexpectedly succeeded through an obstruction");
    history.endRequest("write-failed");
    check(workspace.history().deleteRecursively()&&backup.moveFileTo(workspace.history()),"Could not restore original history after failure");
    check(str(turn(history,0),"status")=="interrupted","Terminal worker failure kept pending request alive indefinitely");
    history.begin(id,"retry-after-write","Retry","a.flows",1);history.finish(id,"retry-after-write","done","Recovered");
    history.archive(id);check(static_cast<bool>(get(selected(history),"archived")),"Recovered conversation still cannot archive");
}
void capacity_limits(){
    Workspace workspace;AssistantHistory history;history.setWorkspace(workspace.directory);const auto id=history.create();
    for(int i=0;i<100;++i){const auto request="turn-"+String(i);history.begin(id,request,"Question","a.flows",1);history.finish(id,request,"done","Answer");}
    const auto before=workspace.history().loadFileAsString();rejects([&]{history.begin(id,"too-many","Question","a.flows",1);},"Exceeded per-conversation turn cap");
    check(workspace.history().loadFileAsString()==before,"Capacity failure erased saved conversation");
    const auto next=history.create();const String large=String::repeatedString("x",1024*1024);
    history.begin(next,"large-1","question","a.flows",1);history.finish(next,"large-1","done",large,large);
    history.begin(next,"large-2","question","a.flows",1);history.finish(next,"large-2","done",large,large);
    history.begin(next,"large-3","question","a.flows",1);history.finish(next,"large-3","done",large,large);
    rejects([&]{history.begin(next,"over-bytes","question","a.flows",1);},"Ignored completion-space byte reservation");
    history.remove(next);history.purge(next);history.begin(history.create(),"after-purge","question","a.flows",1);
}
}
int main(){try{persistence_and_lifecycle();routed_completion_and_context();interrupted_recovery_and_corruption();terminal_save_failure_recovery();capacity_limits();std::cout<<"Assistant history passed: atomic persistence, rename/archive/Trash/restore/purge, multi-instance merge, correct completion routing, bounded paired context, interrupted recovery, corruption preservation and limits.\n";return 0;}catch(const std::exception& exception){std::cerr<<"Assistant history failure: "<<exception.what()<<'\n';return 1;}}
