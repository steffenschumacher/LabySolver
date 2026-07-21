#include "../CoordinatorCheckpoint.hpp"
#include "test_util.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>

static Seed makeSeed(uint64_t id, uint8_t marker) {
    Seed seed{}; seed.id=id; seed.depth=4; seed.state.boardBytes[0]=marker;
    for(size_t i=0;i<seed.depth;++i){seed.moves[i].insertPoint=(marker+i)%10;seed.moves[i].orientation=i%4;}
    return seed;
}

int main(){
 namespace fs=std::filesystem;
 const auto nonce=std::chrono::steady_clock::now().time_since_epoch().count();
 const fs::path directory=fs::temp_directory_path()/("labysolver-checkpoint-"+std::to_string(nonce));
 fs::create_directories(directory); const fs::path path=directory/"coordinator.chk";
 {
  durable::CoordinatorCheckpoint checkpoint(path);
  for(uint64_t ordinal=1;ordinal<=3;++ordinal){Seed seed=makeSeed(0,ordinal+10);CHECK(checkpoint.registerGeneratedSeed(ordinal,seed));CHECK(seed.id==ordinal);}
  checkpoint.markCompleted(2); auto snapshot=checkpoint.snapshot();
  CHECK(snapshot.generatedSeeds==3);CHECK(snapshot.pending.size()==2);CHECK(snapshot.pending[0].id==1);CHECK(snapshot.pending[1].id==3);CHECK(!fs::exists(path.string()+".tmp"));
 }
 {
  durable::CoordinatorCheckpoint recovered(path); auto snapshot=recovered.snapshot();
  CHECK(snapshot.pending.size()==2);CHECK(snapshot.pending[0].state.boardBytes[0]==11);CHECK(snapshot.pending[1].state.boardBytes[0]==13);
  for(uint64_t ordinal=1;ordinal<=3;++ordinal){Seed replay=makeSeed(0,99);CHECK(!recovered.registerGeneratedSeed(ordinal,replay));CHECK(replay.id==ordinal);}
  Seed fourth=makeSeed(0,14);CHECK(recovered.registerGeneratedSeed(4,fourth));recovered.markMasterFinished();recovered.markSolutionFound();
 }
 {
  durable::CoordinatorCheckpoint recovered(path);auto snapshot=recovered.snapshot();CHECK(snapshot.generatedSeeds==4);CHECK(snapshot.pending.size()==3);CHECK(snapshot.masterFinished);CHECK(snapshot.solutionFound);
 }
 bool mismatch=false;try{durable::CoordinatorCheckpoint wrongSearch(path,42);}catch(const durable::CheckpointError&){mismatch=true;}CHECK(mismatch);
 // A power loss before atomic replacement can leave a torn .tmp file; the
 // last committed snapshot remains authoritative and recoverable.
 {std::ofstream torn(path.string()+".tmp",std::ios::binary|std::ios::trunc);torn<<"partial";}
 {durable::CoordinatorCheckpoint afterTornWrite(path);CHECK(afterTornWrite.snapshot().generatedSeeds==4);}
 fs::remove(path.string()+".tmp");
 {
  std::fstream file(path,std::ios::binary|std::ios::in|std::ios::out);file.seekg(-1,std::ios::end);char byte=0;file.read(&byte,1);byte^=0x5a;file.seekp(-1,std::ios::end);file.write(&byte,1);
 }
 bool rejected=false;try{durable::CoordinatorCheckpoint corrupt(path);}catch(const durable::CheckpointError&){rejected=true;}CHECK(rejected);
 fs::remove_all(directory);REPORT();return 0;
}
