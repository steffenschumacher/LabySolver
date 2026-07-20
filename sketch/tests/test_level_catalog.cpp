#include "../LevelCatalog.hpp"
#include "test_util.hpp"
#include <array>
using namespace laby;
int main(){
 std::array<unsigned,8> pushes{}; std::array<unsigned,5> goals{}; unsigned allowed=0;
 for(size_t n=1;n<=LEVEL_CATALOG.size();++n){auto l=loadLevel(n); CHECK(l.initial.ladybug()<BOARD_CELLS); CHECK(l.initial.goalCount()>=1&&l.initial.goalCount()<=4); ++pushes[l.maxPushes]; ++goals[l.initial.goalCount()]; allowed+=l.mayPushPlayerOut;}
 CHECK(pushes[2]==9);CHECK(pushes[3]==5);CHECK(pushes[4]==3);CHECK(pushes[5]==3);CHECK(pushes[6]==9);CHECK(pushes[7]==11);
 CHECK(goals[1]==6);CHECK(goals[2]==4);CHECK(goals[3]==7);CHECK(goals[4]==23);CHECK(allowed==34); REPORT(); return 0;
}
