#include "gtest/gtest.h"

#include "Phase1Storage.h"
#include "ReadDisturbanceModel.h"

using namespace DRAMSim;

namespace
{
bool injectStorage(unsigned bank, unsigned row, unsigned burstColumn, unsigned bit, bool parity, void* context)
{
    if (parity)
        return false;
    Phase1Storage* storage = static_cast<Phase1Storage*>(context);
    Phase1StorageKey key;
    key.actual_bank = bank;
    key.row = row;
    key.burst_column = burstColumn;
    if (!storage->contains(key))
        return false;
    storage->injectDataBit(key, bit);
    return true;
}
}  // namespace

TEST(Phase3M2R, NeighborActAndVictimRefresh)
{
    ReadDisturbanceModel model;
    model.configure(1, 1.0, "HBM2");
    model.forceRow(0, 10, 4.0, 1.0);
    for (int i = 0; i < 7; ++i)
        model.noteActivate(0, 11);
    EXPECT_DOUBLE_EQ(7.0, model.disturbance(0, 10));
    EXPECT_EQ(0u, model.flipCount());
    model.noteRefresh(0, 10);
    EXPECT_DOUBLE_EQ(0.0, model.disturbance(0, 10));
    for (int i = 0; i < 7; ++i)
        model.noteActivate(0, 11);
    EXPECT_DOUBLE_EQ(7.0, model.disturbance(0, 10));
    EXPECT_EQ(0u, model.cumulativeFlips());
    model.noteActivate(0, 11);
    EXPECT_EQ(1u, model.cumulativeFlips());
    EXPECT_EQ(1u, model.firstCrossings());
}

TEST(Phase3M2R, InjectionRequiresWrittenData)
{
    Phase1Storage storage;
    Phase1StorageKey key;
    key.row = 10;
    key.burst_column = 0;
    storage.initialize(key, 0x1111ull);
    ReadDisturbanceModel model;
    model.configure(3, 1.0, "HBM2");
    model.setInjector(&injectStorage, &storage);
    model.forceRow(0, 10, 4.0, 1.0);
    model.forceRow(0, 12, 4.0, 1.0);
    for (int i = 0; i < 8; ++i)
        model.noteActivate(0, 11);
    EXPECT_EQ(1u, model.cumulativeFlips());
    EXPECT_EQ(1u, model.flipCount());
    for (int i = 0; i < 8; ++i)
        model.noteActivate(0, 13);
    EXPECT_EQ(1u, model.cumulativeFlips());
}
