# Teleport locations

Locators that `teleport` (scenario step or live command) lands on and settles at.
Checked on Windows (Vulkan, 30 fps cap) on 2026-11-08: one teleport per
location, back to back from a Downtown start, each arriving within 1 m on the
first attempt (`out/re/teleport_survey_map.txt`, not in the repo).

- The names are the game's internal locator names, not the pause map's labels.
  The ones confirmed against the pause map by hand are noted.
- Industrial destinations load the Industrial map first: ~11 s, against ~3 s
  for Downtown and University.
- Districts without a known map key (the skate school, the separate skate
  parks, DLC areas) are not supported yet: the teleport arrives before
  their map is loaded and the skater falls through the world.

| Location | District | Position (x, y, z) | Map label |
|---|---|---|---|
| `Z_DT_Aletown` | DownTown | -183.4, 12.3, 433.2 |  |
| `Z_DT_Billboard_2` | DownTown | 54.0, 30.2, 32.3 |  |
| `Z_DT_CanalPlaza` | DownTown | -130.8, 48.9, -197.9 |  |
| `Z_DT_CarLot` | DownTown | 172.7, 24.5, 396.5 |  |
| `Z_DT_CityCenter` | DownTown | -187.3, 30.9, 182.5 |  |
| `Z_DT_CorePlaza` | DownTown | 55.4, 30.8, 33.5 |  |
| `Z_DT_CubeTower` | DownTown | 297.3, 55.9, -92.8 |  |
| `Z_DT_HotelPlaza` | DownTown | 149.0, 23.3, 295.8 |  |
| `Z_DT_Mall` | DownTown | -266.0, 23.5, 389.5 |  |
| `Z_DT_Memorial` | DownTown | -254.3, 4.4, 660.6 |  |
| `Z_DT_MidTownpromenade` | DownTown | -114.1, 30.8, 49.4 |  |
| `Z_DT_Park` | DownTown | 42.8, 19.2, 472.9 |  |
| `Z_DT_SkatePark` | DownTown | -254.1, 40.5, 32.4 |  |
| `Z_DT_SpillwayStart` | DownTown | 121.6, 62.0, -191.7 |  |
| `Z_DT_Streets` | DownTown | 11.5, 56.2, -172.7 |  |
| `Z_DT_Tresoutta` | DownTown | -93.7, 30.7, 272.2 | Tresoutta Plaza |
| `Z_DT_breweryHOMstart` | DownTown | -8.4, 21.3, 366.6 |  |
| `Z_DT_parkadeHOMstart` | DownTown | 326.8, 66.0, 64.3 |  |
| `Z_DT_parkadeStart` | DownTown | 382.0, 71.0, -39.6 |  |
| `Z_DT_resPlaza` | DownTown | 295.7, 28.6, 388.5 |  |
| `Z_DT_upTown` | DownTown | 243.5, 64.4, -221.6 |  |
| `freeskate_downtown_central` | DownTown | 56.6, 30.4, 45.1 |  |
| `freeskate_downtown_commercial` | DownTown | -308.1, 18.6, 391.2 |  |
| `freeskate_downtown_memorial` | DownTown | -423.8, 24.2, 958.4 |  |
| `freeskate_downtown_north` | DownTown | 398.9, 73.3, -5.1 |  |
| `freeskate_downtown_south` | DownTown | 292.5, 27.7, 402.7 |  |
| `freeskate_downtown_west` | DownTown | -169.9, 49.1, -111.3 |  |
| `Z_IN_Billboard_3` | Industrial | -89.2, -3.5, 26.8 |  |
| `Z_IN_Billboard_5` | Industrial | -374.4, 0.3, -226.6 |  |
| `Z_Ind_2nd_N_Navy` | Industrial | -189.6, -2.2, 75.3 |  |
| `Z_Ind_GhettoSpot` | Industrial | -516.9, 3.6, -294.5 |  |
| `Z_Ind_Loading_Docks` | Industrial | -458.1, 0.6, -99.3 |  |
| `Z_Ind_NewFactory` | Industrial | -39.0, 12.2, -398.4 |  |
| `Z_Ind_NewFactoryRoof` | Industrial | -182.0, 36.9, -455.1 |  |
| `Z_Ind_OldFactory` | Industrial | 97.7, 6.9, 0.1 | The Old Factory |
| `Z_Ind_Quarry` | Industrial | 350.1, 68.8, -122.5 |  |
| `Z_Ind_ReclaimedHastings` | Industrial | -89.2, 1.0, -138.3 |  |
| `Z_Ind_Spillway` | Industrial | 782.4, 140.8, -424.0 |  |
| `Z_Ind_Spillway_Bottom` | Industrial | -339.3, 14.1, -450.6 |  |
| `Z_Ind_TankerShip` | Industrial | -440.6, 22.3, 36.5 |  |
| `Z_Ind_TeamHQ` | Industrial | -630.6, 6.5, -358.3 | Team HQ |
| `Z_Ind_dryDocks` | Industrial | -676.4, 2.9, -253.8 |  |
| `Z_Ind_loadingDock` | Industrial | -391.4, 2.2, -114.8 |  |
| `freeskate_indu_docks` | Industrial | -762.2, 1.9, -216.5 |  |
| `freeskate_indu_north` | Industrial | -44.4, 37.5, -462.9 |  |
| `freeskate_indu_quarry` | Industrial | 376.5, 66.0, -24.5 |  |
| `freeskate_indu_south` | Industrial | -110.9, -1.8, -38.3 |  |
| `freeskate_indu_spillway` | Industrial | 780.5, 141.5, -420.2 |  |
| `Z_UN_Billboard_1` | University | 282.3, 64.2, -51.5 |  |
| `Z_UN_Billboard_4` | University | -223.7, 194.2, -725.5 |  |
| `Z_UN_CampusEntrance` | University | -101.1, 64.3, -178.1 |  |
| `Z_UN_CampusStart` | University | 407.0, 69.9, -336.4 | (the user's first map teleport, University campus) |
| `Z_UN_ClockTower` | University | 171.0, 64.0, -244.5 | Clock Tower |
| `Z_UN_CommunitySkatePark` | University | 338.9, 4.0, 368.9 |  |
| `Z_UN_LowerCampus` | University | 180.3, 60.4, -85.0 |  |
| `Z_UN_ObservatoryRoad` | University | -397.1, 222.6, -831.1 |  |
| `Z_UN_ResRoads` | University | -148.7, 45.6, 121.0 |  |
| `Z_UN_SkatePath` | University | -376.7, 223.2, -810.1 |  |
| `Z_UN_SpillwayStart` | University | -380.5, 222.6, -875.9 |  |
| `Z_UN_StadiumCampus` | University | 294.3, 64.0, -76.9 |  |
| `Z_UN_TrainingExterior` | University | 308.7, 74.0, -439.0 |  |
| `Z_UN_observatory` | University | -553.0, 222.6, -831.1 |  |
| `Z_UniversityMegaPark` | University | 350.5, 140.3, -725.4 |  |
| `Z_UniversityMegaPark1` | University | 333.3, 112.6, -674.0 |  |
| `Z_gpmm_mega_park` | University | 356.6, 132.3, -704.7 |  |
| `freeskate_univ_testdmo` | University | 377.9, 68.8, -293.2 |  |
| `freeskate_university_central` | University | 169.7, 66.1, -199.1 |  |
| `freeskate_university_east` | University | 407.3, 70.9, -329.8 |  |
| `freeskate_university_megapark` | University | 316.7, 141.3, -754.4 |  |
| `freeskate_university_north` | University | -560.6, 224.9, -830.1 |  |
| `freeskate_university_south` | University | 400.2, 6.5, 363.0 |  |
| `freeskate_university_west` | University | 38.3, 157.7, -729.2 |  |

Not usable as start locations (the skater does not settle):

- `Z_DT_HotelPlazaPool` (DownTown): lands in the pool
- `teleport_dwtn_01_toDwtnskpk_locator_01` (DownTown): an area-link trigger (leads into a park or interior)
- `teleport_indu_01_ToTeamHQ_locator_01` (Industrial): an area-link trigger (leads into a park or interior)

Regenerate the locator list from a memory snapshot with `tools/re/registry.py`, and
the district table (`src/harness/teleport_districts.inc`) with
`tools/re/locator_districts.py <game dump>`.
