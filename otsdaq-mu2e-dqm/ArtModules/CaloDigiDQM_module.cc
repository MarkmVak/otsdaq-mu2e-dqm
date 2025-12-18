#include "Offline/CaloConditions/inc/CaloDAQMap.hh"
#include "Offline/ProditionsService/inc/ProditionsHandle.hh"

#include "art/Framework/Core/EDAnalyzer.h"
#include "art/Framework/Core/ModuleMacros.h"
#include "art/Framework/Principal/Event.h"
#include "art/Framework/Services/Registry/ServiceHandle.h"

#include "art_root_io/TFileDirectory.h"
#include "art_root_io/TFileService.h"

#include "canvas/Utilities/InputTag.h"
#include "messagefacility/MessageLogger/MessageLogger.h"

#include "otsdaq-mu2e/ArtModules/HistoSender.hh"

#include "Offline/CaloVisualizer/inc/THMu2eCaloDisk.hh"
#include "Offline/DataProducts/inc/CaloSiPMId.hh"
#include "Offline/RecoDataProducts/inc/CaloDigi.hh"

#include "fhiclcpp/types/Atom.h"
#include "fhiclcpp/types/Sequence.h"
#include "fhiclcpp/types/Table.h"

#include "TH1F.h"
#include "TH2D.h"
#include "TH2I.h"
#include "TProfile.h"
#include "TString.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

/*
 * CaloDigiDQM
 *
 * Detector-aware monitoring for Mu2e calorimeter CaloDigi:
 *  - Per-disk summaries vs contiguous channel index within disk (occupancy, baseline, RMS, peak ADC)
 *  - Global integrity plots (board/channel dist, board-vs-channel occupancy, waveform "first-hit" density)
 *  - Optional disk heatmaps (Amp/Sum/Asym/Baseline/RMS) via THMu2eCaloDisk (mean semantics)
 *  - Optional board summaries and per-channel waveforms (live + first-hit snapshot)
 *
 * Streaming:
 *  - Summary groups every freqDQM
 *  - Waveform groups every freqWaveforms
 */

namespace mu2e
{

  class CaloDigiDQM : public art::EDAnalyzer
  {
  public:
    struct Config
    {
      // otsdaq streaming
      fhicl::Atom<std::string> address{fhicl::Name("address"), "mu2edaq11-data.fnal.gov"};
      fhicl::Atom<int>         port{fhicl::Name("port"), 6000};
      fhicl::Atom<std::string> moduleTag{fhicl::Name("moduleTag"), "CaloDQM"};
      fhicl::Atom<bool>        sendHists{fhicl::Name("sendHists"), false};

      // streaming cadence
      fhicl::Atom<int> freqDQM{fhicl::Name("freqDQM"), 100};
      fhicl::Atom<int> freqWaveforms{fhicl::Name("freqWaveforms"), 0}; // 0 = disable waveform streaming

      // input + detail level
      fhicl::Atom<std::string> caloDigiModuleLabel{fhicl::Name("caloDigiModuleLabel"), "CaloDigi"};
      fhicl::Atom<bool>        enableBoardHistos{fhicl::Name("enableBoardHistos"), true};
      fhicl::Atom<int>         maxBoardHistos{fhicl::Name("maxBoardHistos"), -1}; // per disk; -1 = no limit

      // disk maps
      fhicl::Atom<bool> enableDiskMaps{fhicl::Name("enableDiskMaps"), true};
      fhicl::Sequence<std::string> diskCombines{
        fhicl::Name("diskCombines"), std::vector<std::string>{"asym"}};
    };

    explicit CaloDigiDQM(const art::EDAnalyzer::Table<Config>& config);
    void analyze(art::Event const& event) override;
    void endJob() override;

  private:
    // -----------------------
    // Fixed waveform settings (NO FHiCL)
    // -----------------------
    static constexpr int kWaveformNBins       = 64;
    static constexpr int kWaveformSizeHistMax = 200;

    // -----------------------
    // Map mode infrastructure
    // -----------------------
    enum class MapMode { Amp, Sum, Asym, Baseline, RMS };

    static MapMode parseMode(std::string s)
    {
      std::transform(s.begin(), s.end(), s.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      if(s == "sum")      return MapMode::Sum;
      if(s == "asym")     return MapMode::Asym;
      if(s == "baseline") return MapMode::Baseline;
      if(s == "rms")      return MapMode::RMS;
      return MapMode::Amp;
    }

    static const char* modeSuffix(MapMode m)
    {
      switch(m)
        {
        case MapMode::Amp:      return "Amp";
        case MapMode::Sum:      return "Sum";
        case MapMode::Asym:     return "Asym";
        case MapMode::Baseline: return "Baseline";
        case MapMode::RMS:      return "RMS";
        }
      return "Amp";
    }

    static const char* modeFolder(MapMode m)
    {
      switch(m)
        {
        case MapMode::Amp:      return "DiskAmp";
        case MapMode::Sum:      return "DiskSum";
        case MapMode::Asym:     return "DiskAsym";
        case MapMode::Baseline: return "DiskBaseline";
        case MapMode::RMS:      return "DiskRMS";
        }
      return "DiskAmp";
    }

    void setDiskMapTitles(mu2e::THMu2eCaloDisk* h, int disk, MapMode mode)
    {
      if(!h) return;

      const char* ztitle = "Value [ADC]";
      const char* main   = "SiPM value";

      switch(mode)
        {
        case MapMode::Amp:
          main   = "SiPM amplitude (peak - baseline)";
          ztitle = "Amplitude [ADC]";
          break;
        case MapMode::Sum:
          main   = "Crystal sum (L+R) amplitude";
          ztitle = "L+R [ADC]";
          break;
        case MapMode::Asym:
          main   = "Asymmetry (L-R)/(L+R)";
          ztitle = "Asymmetry";
          h->SetMinimum(-1.0);
          h->SetMaximum(1.0);
          break;
        case MapMode::Baseline:
          main   = "SiPM baseline";
          ztitle = "Baseline [ADC]";
          break;
        case MapMode::RMS:
          main   = "SiPM baseline RMS";
          ztitle = "RMS [ADC]";
          break;
        }

      h->SetTitle(Form("Disk %d - %s", disk, main));
      h->GetZaxis()->SetTitle(ztitle);
      h->SetOption("COLZ L");
      h->SetStats(0);
    }

    // -----------------------
    // Geometry / encoding
    // -----------------------
    static constexpr int kNDisks           = 2;
    static constexpr int kBoardsPerDisk    = 80;
    static constexpr int kChannelsPerBoard = 20;
    static constexpr int kChannelsPerDisk  = kBoardsPerDisk * kChannelsPerBoard;

    static int boardMinForDisk(int disk) { return disk * kBoardsPerDisk; }

    // Contiguous channel index within disk: 0..(kChannelsPerDisk-1)
    static int encodeChannel(int disk, int boardID, int chanID)
    {
      const int bmin = boardMinForDisk(disk);
      return (boardID - bmin) * kChannelsPerBoard + chanID;
    }

    struct EncodedAxisConfig { int nBins; double xMin; double xMax; };

    static EncodedAxisConfig axisForDisk(int /*disk*/)
    {
      return EncodedAxisConfig{kChannelsPerDisk, 0.0, (double)kChannelsPerDisk};
    }

    struct ChannelKey
    {
      int disk{0};
      int board{0};
      int chan{0};

      bool operator<(ChannelKey const& o) const
      {
        if(disk != o.disk)   return disk < o.disk;
        if(board != o.board) return board < o.board;
        return chan < o.chan;
      }
    };

    // -----------------------
    // Disk-map MEAN semantics
    // -----------------------
    static void ensureSize(std::vector<double>& v, size_t idx)
    {
      if(v.size() <= idx) v.resize(idx + 1, 0.0);
    }
    static void ensureSize(std::vector<uint32_t>& v, size_t idx)
    {
      if(v.size() <= idx) v.resize(idx + 1, 0u);
    }

    static constexpr int kMaxSipmIdForMaps_ = 10000; // safety cap
    bool warnedBadSipmId_{false};
    int  nBadSipmId_{0};

    void accDisk(MapMode m, int disk, int sipmId, double val)
    {
      if(disk < 0 || disk >= kNDisks) return;

      if(sipmId < 0 || sipmId >= kMaxSipmIdForMaps_)
        {
          ++nBadSipmId_;
          if(!warnedBadSipmId_)
            {
              warnedBadSipmId_ = true;
              mf::LogWarning("CaloDigiDQM")
                << "Out-of-range sipmId=" << sipmId
                << " (cap=" << kMaxSipmIdForMaps_ << "). Disk-map accumulation will skip these.";
            }
          return;
        }

      auto& sumv = diskSum_[m][disk];
      auto& cntv = diskCnt_[m][disk];

      ensureSize(sumv, (size_t)sipmId);
      ensureSize(cntv, (size_t)sipmId);

      sumv[(size_t)sipmId] += val;
      cntv[(size_t)sipmId] += 1;
    }

    void refreshDiskMaps()
    {
      if(!enableDiskMaps_) return;

      for(auto m : modes_)
        {
          for(int disk = 0; disk < kNDisks; ++disk)
            {
              auto* h = (disk == 0) ? disk0Maps_[m] : disk1Maps_[m];
              if(!h) continue;

              h->Reset("ICESM");
              setDiskMapTitles(h, disk, m);

              auto& sumv = diskSum_[m][disk];
              auto& cntv = diskCnt_[m][disk];

              const size_t n = std::min(sumv.size(), cntv.size());
              for(size_t sipm = 0; sipm < n; ++sipm)
                {
                  if(cntv[sipm] == 0u) continue;
                  h->FillOffline((int)sipm, sumv[sipm] / (double)cntv[sipm]);
                }
            }
        }
    }

    // -----------------------
    // Fixed-bin waveform fill
    // -----------------------
    template <class WaveformT>
    void fillFixedWaveform(TH1F* h, WaveformT const& wf) const
    {
      if(!h) return;

      const int nb = h->GetNbinsX();
      const int n  = std::min<int>((int)wf.size(), nb);

      for(int i = 0; i < n; ++i)  h->SetBinContent(i + 1, (double)wf[(size_t)i]);
      for(int i = n; i < nb; ++i) h->SetBinContent(i + 1, 0.0);
    }

    // -----------------------
    // Waveform-size bookkeeping
    // -----------------------
    struct WaveformSizeStats
    {
      uint32_t first{0}, last{0}, min{0}, max{0};
      uint32_t nSeen{0}, nMismatchToFirst{0}, nTransitions{0};
      uint32_t nTruncated{0}, nPadded{0};
    };

    // -----------------------
    // Helpers
    // -----------------------
    bool modeEnabled(MapMode m) const { return enabledModes_.count(m) != 0; }

    // -----------------------
    // Data members
    // -----------------------
    std::vector<MapMode> modes_;
    std::set<MapMode>    enabledModes_;

    art::InputTag caloDigiTag_;
    std::string   caloDigiModuleLabel_;

    bool        enableBoardHistos_;
    int         maxBoardHistos_;
    int         freqDQM_;
    int         freqWaveforms_;
    std::string address_;
    int         port_;
    std::string moduleTag_;
    bool        sendHists_;

    std::unique_ptr<ots::HistoSender> histSender_;
    int                               eventCounter_{0};
    int                               waveformCounter_{0};
    int                               histSendErrorCount_{0};
    static constexpr int              kMaxSendErrors_ = 10;

    bool enableDiskMaps_{true};

    int nFillDisk0_{0}, nFillDisk1_{0}, nFillMiss_{0};

    std::set<int> boardsSeenDisk0_;
    std::set<int> boardsSeenDisk1_;
    std::set<std::pair<int, int>> warnedBoardsSkipped_;

    // Board-level summaries
    std::map<std::pair<int, int>, std::map<std::string, TH1*>>          boardHistos_;
    std::map<std::pair<int, int>, std::unique_ptr<art::TFileDirectory>> cachedHistosDirs_;
    std::map<std::pair<int, int>, std::unique_ptr<art::TFileDirectory>> cachedChannelsDirs_;

    // Per-channel waveforms
    std::map<ChannelKey, TH1F*> channelWaveformHistos_;
    std::map<ChannelKey, TH1F*> singleWaveformHistos_;
    std::set<ChannelKey>        channelWaveformStored_;
    std::map<ChannelKey, WaveformSizeStats> wfSizeStats_;

    // TFileService folders
    std::unique_ptr<art::TFileDirectory> disk0Dir_;
    std::unique_ptr<art::TFileDirectory> disk1Dir_;
    std::unique_ptr<art::TFileDirectory> globalDir_;

    // Disk maps per mode
    std::map<MapMode, art::TFileDirectory>   diskMapDirs_;
    std::map<MapMode, mu2e::THMu2eCaloDisk*> disk0Maps_;
    std::map<MapMode, mu2e::THMu2eCaloDisk*> disk1Maps_;

    // Disk-map running mean buffers: [mode][disk][sipm]
    std::map<MapMode, std::array<std::vector<double>, kNDisks>>   diskSum_;
    std::map<MapMode, std::array<std::vector<uint32_t>, kNDisks>> diskCnt_;

    // Global/summaries
    TH1F*     h_asymmetry{nullptr};
    TProfile* h_baseline_vs_disk{nullptr};

    TH1F*     h_occupancy_disk0_{nullptr};
    TH1F*     h_occupancy_disk1_{nullptr};
    TProfile* h_baseline_disk0_{nullptr};
    TProfile* h_baseline_disk1_{nullptr};
    TProfile* h_rms_disk0_{nullptr};
    TProfile* h_rms_disk1_{nullptr};
    TProfile* h_maxval_disk0_{nullptr};
    TProfile* h_maxval_disk1_{nullptr};

    TH1F* h_global_channel_dist_{nullptr};
    TH1F* h_global_board_dist_{nullptr};

    TH2I* h_global_board_vs_channel_{nullptr};
    TH2D* h_global_waveform_density_{nullptr};

    TH1F* h_waveform_size_{nullptr};

    // Electronics mapping
    mu2e::ProditionsHandle<mu2e::CaloDAQMap> _calodaqconds_h;
  };

  static TString channelLabel(int boardID, int chanID, int rawId, int sipmId)
  {
    return Form("B%03d C%02d (raw: %d, offline: %d)", boardID, chanID, rawId, sipmId);
  }

  // ===========================
  // Constructor
  // ===========================
  CaloDigiDQM::CaloDigiDQM(const art::EDAnalyzer::Table<Config>& config)
    : art::EDAnalyzer{config}
    , caloDigiTag_{config().caloDigiModuleLabel()}
    , caloDigiModuleLabel_(config().caloDigiModuleLabel())
    , enableBoardHistos_(config().enableBoardHistos())
    , maxBoardHistos_(config().maxBoardHistos())
    , freqDQM_(config().freqDQM())
    , freqWaveforms_(config().freqWaveforms())
    , address_(config().address())
    , port_(config().port())
    , moduleTag_(config().moduleTag())
    , sendHists_(config().sendHists())
    , enableDiskMaps_(config().enableDiskMaps())
  {

    // Parse enabled disk map modes (fallback to {"asym"})
    std::vector<std::string> rawModes = config().diskCombines();
    if(rawModes.empty()) rawModes = {"asym"};

    modes_.reserve(rawModes.size());
    for(auto& s : rawModes)
      {
        const auto m = parseMode(s);
        modes_.push_back(m);
        enabledModes_.insert(m);
      }

    // HistoSender (optional)
    if(sendHists_) histSender_ = std::make_unique<ots::HistoSender>(address_, port_);

    // Top-level output folders
    art::ServiceHandle<art::TFileService> tfs;
    disk0Dir_  = std::make_unique<art::TFileDirectory>(tfs->mkdir("Disk0"));
    disk1Dir_  = std::make_unique<art::TFileDirectory>(tfs->mkdir("Disk1"));
    globalDir_ = std::make_unique<art::TFileDirectory>(tfs->mkdir("Global_Histograms"));

    // Axes per disk
    auto axisD0 = axisForDisk(0);
    auto axisD1 = axisForDisk(1);

    // Occupancy vs contiguous channel index
    h_occupancy_disk0_ =
      disk0Dir_->make<TH1F>("h_occ_d0", "Occupancy (Disk 0)", axisD0.nBins, axisD0.xMin, axisD0.xMax);
    h_occupancy_disk0_->GetXaxis()->SetTitle("Channel index within disk ((boardID-boardMin)*20 + chanID)");
    h_occupancy_disk0_->GetYaxis()->SetTitle("Hit Count");

    h_occupancy_disk1_ =
      disk1Dir_->make<TH1F>("h_occ_d1", "Occupancy (Disk 1)", axisD1.nBins, axisD1.xMin, axisD1.xMax);
    h_occupancy_disk1_->GetXaxis()->SetTitle("Channel index within disk ((boardID-boardMin)*20 + chanID)");
    h_occupancy_disk1_->GetYaxis()->SetTitle("Hit Count");

    // Global 2D occupancy (filled every hit)
    h_global_board_vs_channel_ =
      globalDir_->make<TH2I>("h_board_vs_channel", "Board vs Channel Occupancy", 160, 0, 160, 20, 0, 20);
    h_global_board_vs_channel_->GetXaxis()->SetTitle("Board ID");
    h_global_board_vs_channel_->GetYaxis()->SetTitle("Channel ID");

    // Global waveform density (from first-hit snapshot per channel)
    h_global_waveform_density_ =
      globalDir_->make<TH2D>("h_waveform_density", "Waveform Density (first-hit per channel)",
                             150, 0, 150, 400, 2000, 4095);
    h_global_waveform_density_->GetXaxis()->SetTitle("Tick");
    h_global_waveform_density_->GetYaxis()->SetTitle("ADC Value");

    // waveform.size() distribution
    const int sizeMax = std::max(10, kWaveformSizeHistMax);
    h_waveform_size_ =
      globalDir_->make<TH1F>("h_waveform_size", "Waveform size distribution", sizeMax, 0, sizeMax);
    h_waveform_size_->GetXaxis()->SetTitle("waveform.size() [samples]");
    h_waveform_size_->GetYaxis()->SetTitle("Count");

    auto makeValueVsEncoded =
      [&](art::TFileDirectory& dir, const char* name, const char* title, int disk, const char* yTitle) -> TProfile* {
        EncodedAxisConfig ax = axisForDisk(disk);
        auto* h = dir.make<TProfile>(name, title, ax.nBins, ax.xMin, ax.xMax);
        h->SetMarkerStyle(20);
        h->GetXaxis()->SetTitle("Channel index within disk ((boardID-boardMin)*20 + chanID)");
        h->GetYaxis()->SetTitle(yTitle);
        return h;
      };

    h_baseline_disk0_ = makeValueVsEncoded(*disk0Dir_, "h_base_d0", "Baseline (Disk 0)", 0, "Mean Baseline [ADC]");
    h_baseline_disk1_ = makeValueVsEncoded(*disk1Dir_, "h_base_d1", "Baseline (Disk 1)", 1, "Mean Baseline [ADC]");

    h_rms_disk0_ = makeValueVsEncoded(*disk0Dir_, "h_rms_d0", "RMS (Disk 0)", 0, "Mean RMS [ADC]");
    h_rms_disk1_ = makeValueVsEncoded(*disk1Dir_, "h_rms_d1", "RMS (Disk 1)", 1, "Mean RMS [ADC]");

    h_maxval_disk0_ = makeValueVsEncoded(*disk0Dir_, "h_max_d0", "Max ADC (Disk 0)", 0, "Mean Peak ADC [ADC]");
    h_maxval_disk1_ = makeValueVsEncoded(*disk1Dir_, "h_max_d1", "Max ADC (Disk 1)", 1, "Mean Peak ADC [ADC]");

    // Disk-level baseline comparison
    h_baseline_vs_disk = globalDir_->make<TProfile>("h_base_vs_d", "Baseline vs Disk", 2, 0, 2);
    h_baseline_vs_disk->GetXaxis()->SetBinLabel(1, "Disk 0");
    h_baseline_vs_disk->GetXaxis()->SetBinLabel(2, "Disk 1");
    h_baseline_vs_disk->GetYaxis()->SetTitle("Mean Baseline [ADC]");

    // Global asymmetry distribution
    h_asymmetry = globalDir_->make<TH1F>("h_asym", "Left-Right Asymmetry", 100, -1.0, 1.0);
    h_asymmetry->GetXaxis()->SetTitle("(L - R)/(L + R)");
    h_asymmetry->GetYaxis()->SetTitle("Frequency");

    // Global ID distributions
    h_global_channel_dist_ = globalDir_->make<TH1F>("h_channel_dist", "Global Channel Distribution", 20, 0, 20);
    h_global_channel_dist_->GetXaxis()->SetTitle("Channel ID");
    h_global_channel_dist_->GetYaxis()->SetTitle("Frequency");

    h_global_board_dist_ = globalDir_->make<TH1F>("h_board_dist", "Global Board Distribution", 160, 0, 160);
    h_global_board_dist_->GetXaxis()->SetTitle("Board ID");
    h_global_board_dist_->GetYaxis()->SetTitle("Frequency");

    // Disk maps per enabled mode
    if(enableDiskMaps_)
      {
        for(auto m : modes_)
          {
            const char* suf    = modeSuffix(m);
            const char* folder = modeFolder(m);

            auto& modeDir = diskMapDirs_.try_emplace(m, globalDir_->mkdir(folder)).first->second;

            std::string key0   = Form("disk0_%s", suf);
            std::string key1   = Form("disk1_%s", suf);
            std::string title0 = Form("Disk 0 - %s", suf);
            std::string title1 = Form("Disk 1 - %s", suf);

            auto* d0 = modeDir.makeAndRegister<mu2e::THMu2eCaloDisk>(key0.c_str(), title0.c_str(), key0.c_str(), title0.c_str(), 0);
            auto* d1 = modeDir.makeAndRegister<mu2e::THMu2eCaloDisk>(key1.c_str(), title1.c_str(), key1.c_str(), title1.c_str(), 1);

            setDiskMapTitles(d0, 0, m);
            setDiskMapTitles(d1, 1, m);
            disk0Maps_[m] = d0;
            disk1Maps_[m] = d1;
          }
      }
  }

  // ===========================
  // analyze()
  // ===========================
  void CaloDigiDQM::analyze(art::Event const& event)
  {
    const auto& caloDigis    = *event.getValidHandle<CaloDigiCollection>(caloDigiTag_);
    const auto& calodaqconds = _calodaqconds_h.get(event.id());

    // Per-event cache for L/R pairing
    struct SipmFeat
    {
      double amp{0.0};      // baseline-subtracted peak
      double baseline{0.0};
      double rms{0.0};
      double ampRaw{0.0};   // raw peak
      int    disk{-1};
      int    board{-1};
      int    chan{-1};
    };

    std::map<int, SipmFeat> featBySipm;
    std::set<int> pairedCrystals;

    // Threshold to avoid asym blow-ups at tiny denom
    static constexpr double kMinDenomForAsym = 5.0;

    for(const auto& digi : caloDigis)
      {
        const auto& waveform = digi.waveform();

        if(h_waveform_size_) h_waveform_size_->Fill((int)waveform.size());

        if(waveform.size() < 5 || digi.peakpos() >= (int)waveform.size()) continue;

        const float baseline =
          std::accumulate(waveform.begin(), waveform.begin() + 5, 0.0f) / 5.0f;

        const float mean_sq =
          std::inner_product(waveform.begin(), waveform.begin() + 5, waveform.begin(), 0.0f) / 5.0f;

        const float rms = (mean_sq > baseline * baseline) ? std::sqrt(mean_sq - baseline * baseline) : 0.0f;

        if(!std::isfinite(baseline) || !std::isfinite(rms)) continue;

        const int   sipmId = digi.SiPMID();
        if(sipmId < 0) continue;

        const float ampRaw = waveform[digi.peakpos()];
        const double amp   = (double)ampRaw - (double)baseline;

        // SiPMID -> rawId -> board/channel/disk
        const int rawId = calodaqconds.rawId(mu2e::CaloSiPMId(sipmId)).id();
        if(rawId == 9999) continue;

        // Extra guards
        if(rawId < 0) continue;

        const int boardID = rawId / kChannelsPerBoard;
        const int chanID  = rawId % kChannelsPerBoard;

        if(chanID < 0 || chanID >= kChannelsPerBoard) continue;
        if(boardID < 0 || boardID >= (kBoardsPerDisk * kNDisks))
          {
            ++nFillMiss_;
            continue;
          }

        const int disk = boardID / kBoardsPerDisk;
        if(disk < 0 || disk >= kNDisks)
          {
            ++nFillMiss_;
            continue;
          }

        // Enforce disk-partition assumption explicitly
        const int bmin = boardMinForDisk(disk);
        if(boardID < bmin || boardID >= bmin + kBoardsPerDisk)
          {
            ++nFillMiss_;
            continue;
          }

        const int encoded = encodeChannel(disk, boardID, chanID);

        if(disk == 0) ++nFillDisk0_;
        else          ++nFillDisk1_;

        // Global integrity
        h_global_board_dist_->Fill(boardID);
        h_global_channel_dist_->Fill(chanID);
        h_global_board_vs_channel_->Fill(boardID, chanID);

        // Per-disk 1D summaries
        (disk == 0 ? h_occupancy_disk0_ : h_occupancy_disk1_)->Fill(encoded);
        (disk == 0 ? h_baseline_disk0_  : h_baseline_disk1_)->Fill(encoded, baseline);
        (disk == 0 ? h_rms_disk0_       : h_rms_disk1_)->Fill(encoded, rms);
        (disk == 0 ? h_maxval_disk0_    : h_maxval_disk1_)->Fill(encoded, ampRaw);
        h_baseline_vs_disk->Fill(disk == 0 ? 0.5 : 1.5, baseline);

        // Disk-map running means (only if enabled by config list)
        if(enableDiskMaps_)
          {
            if(modeEnabled(MapMode::Amp))      accDisk(MapMode::Amp,      disk, sipmId, amp);
            if(modeEnabled(MapMode::Baseline)) accDisk(MapMode::Baseline, disk, sipmId, baseline);
            if(modeEnabled(MapMode::RMS))      accDisk(MapMode::RMS,      disk, sipmId, rms);
          }

        featBySipm[sipmId] = SipmFeat{amp, baseline, rms, (double)ampRaw, disk, boardID, chanID};

        // Pair Sum/Asym once per crystal
        const int crystalId = sipmId / 2;
        if(pairedCrystals.count(crystalId) == 0)
          {
            const int evenId = 2 * crystalId;
            const int oddId  = evenId + 1;

            auto itL = featBySipm.find(evenId);
            auto itR = featBySipm.find(oddId);

            if(itL != featBySipm.end() && itR != featBySipm.end())
              {
                pairedCrystals.insert(crystalId);

                const double L = itL->second.amp;
                const double R = itR->second.amp;

                const double denom = L + R;

                // Skip asym/sum when denom is tiny -> avoids noise dominating maps and h_asymmetry
                if(std::abs(denom) > kMinDenomForAsym)
                  {
                    const double sumLR = denom;
                    const double asym  = (L - R) / denom;

                    h_asymmetry->Fill(asym);

                    if(enableDiskMaps_)
                      {
                        const int dL = itL->second.disk;
                        const int dR = itR->second.disk;

                        // FIX: fill each SiPM into its own disk (no forced dL)
                        if(modeEnabled(MapMode::Sum))
                          {
                            accDisk(MapMode::Sum,  dL, evenId, sumLR);
                            accDisk(MapMode::Sum,  dR, oddId,  sumLR);
                          }
                        if(modeEnabled(MapMode::Asym))
                          {
                            accDisk(MapMode::Asym, dL, evenId, asym);
                            accDisk(MapMode::Asym, dR, oddId,  asym);
                          }

                        if(dL != dR)
                          {
                            mf::LogWarning("CaloDigiDQM")
                              << "Disk mismatch for paired crystal " << crystalId
                              << " (SiPM " << evenId << " in disk " << dL
                              << ", SiPM " << oddId  << " in disk " << dR << ").";
                          }
                      }
                  }
              }
          }

        // -----------------------
        // Board-level histograms
        // -----------------------
        if(!enableBoardHistos_) continue;

        const std::pair<int, int> boardKey = std::make_pair(disk, boardID);
        const bool boardKnown = (boardHistos_.find(boardKey) != boardHistos_.end());
        auto& boardsSeen = (disk == 0) ? boardsSeenDisk0_ : boardsSeenDisk1_;

        bool allowBoard = true;
        if(!boardKnown && maxBoardHistos_ >= 0 && (int)boardsSeen.size() >= maxBoardHistos_)
          allowBoard = false;

        if(!allowBoard)
          {
            if(!warnedBoardsSkipped_.count(boardKey))
              {
                mf::LogInfo("CaloDigiDQM")
                  << "Skipping board-level histos/waveforms for D" << disk << " B" << boardID
                  << " due to maxBoardHistos(per disk)=" << maxBoardHistos_ << ".";
                warnedBoardsSkipped_.insert(boardKey);
              }
            continue;
          }

        if(!boardKnown) boardsSeen.insert(boardID);

        if(cachedHistosDirs_.find(boardKey) == cachedHistosDirs_.end())
          {
            art::TFileDirectory boardDir =
              (disk == 0 ? *disk0Dir_ : *disk1Dir_).mkdir("Board_" + std::to_string(boardID));

            cachedHistosDirs_[boardKey]    = std::make_unique<art::TFileDirectory>(boardDir.mkdir("Histograms"));
            cachedChannelsDirs_[boardKey]  = std::make_unique<art::TFileDirectory>(boardDir.mkdir("Channels"));
          }

        art::TFileDirectory& histosDir = *cachedHistosDirs_[boardKey];
        auto& histos = boardHistos_[boardKey];

        if(histos.empty())
          {
            histos["occ"] = histosDir.make<TH1F>(Form("D%d_B%03d_Occupancy", disk, boardID),
                                                 Form("Occupancy for D%d B%03d", disk, boardID),
                                                 kChannelsPerBoard, 0, kChannelsPerBoard);
            histos["occ"]->GetXaxis()->SetTitle("Channel ID");
            histos["occ"]->GetYaxis()->SetTitle("Count");

            histos["base"] = histosDir.make<TProfile>(Form("D%d_B%03d_Baseline", disk, boardID),
                                                      Form("Baseline for D%d B%03d", disk, boardID),
                                                      kChannelsPerBoard, 0, kChannelsPerBoard);

            histos["rms"]  = histosDir.make<TProfile>(Form("D%d_B%03d_RMS", disk, boardID),
                                                      Form("RMS for D%d B%03d", disk, boardID),
                                                      kChannelsPerBoard, 0, kChannelsPerBoard);

            histos["max"]  = histosDir.make<TProfile>(Form("D%d_B%03d_Max", disk, boardID),
                                                      Form("Max for D%d B%03d", disk, boardID),
                                                      kChannelsPerBoard, 0, kChannelsPerBoard);

            for(auto key : {"base", "rms", "max"})
              {
                histos[key]->GetXaxis()->SetTitle("Channel ID");
                static_cast<TProfile*>(histos[key])->SetMarkerStyle(20);
              }

            histos["base"]->GetYaxis()->SetTitle("Mean Baseline [ADC]");
            histos["rms"]->GetYaxis()->SetTitle("Mean RMS [ADC]");
            histos["max"]->GetYaxis()->SetTitle("Mean Peak ADC [ADC]");
          }

        histos["occ"]->Fill(chanID);
        static_cast<TProfile*>(histos["base"])->Fill(chanID, baseline);
        static_cast<TProfile*>(histos["rms"])->Fill(chanID, rms);
        static_cast<TProfile*>(histos["max"])->Fill(chanID, ampRaw);

        // -----------------------
        // Waveforms (fixed binning)
        // -----------------------
        ChannelKey chKey{disk, boardID, chanID};

        // size stats
        {
          const uint32_t sz = (uint32_t)waveform.size();
          auto& st = wfSizeStats_[chKey];

          if(st.nSeen == 0) st.first = st.last = st.min = st.max = sz;
          else
            {
              if(sz != st.first) st.nMismatchToFirst++;
              if(sz != st.last)  st.nTransitions++;
              if(sz < st.min)    st.min = sz;
              if(sz > st.max)    st.max = sz;
              st.last = sz;
            }

          st.nSeen++;
          if(sz > (uint32_t)kWaveformNBins) st.nTruncated++;
          else if(sz < (uint32_t)kWaveformNBins) st.nPadded++;
        }

        if(!channelWaveformHistos_.count(chKey))
          {
            TString cname  = Form("D%d_B%03d_C%02d_Waveform", disk, boardID, chanID);
            TString ctitle = Form("Live Waveform for %s", channelLabel(boardID, chanID, rawId, sipmId).Data());

            channelWaveformHistos_[chKey] = histosDir.make<TH1F>(cname, ctitle, kWaveformNBins, 0, kWaveformNBins);
            channelWaveformHistos_[chKey]->GetYaxis()->SetTitle("ADC Value");
            channelWaveformHistos_[chKey]->GetXaxis()->SetTitle("Tick");
          }
        fillFixedWaveform(channelWaveformHistos_[chKey], waveform);

        if(channelWaveformStored_.count(chKey) == 0)
          {
            art::TFileDirectory& chanDir = *cachedChannelsDirs_[boardKey];

            TString cname  = Form("D%d_B%03d_C%02d_FirstHit", disk, boardID, chanID);
            TString ctitle = Form("First-Hit Waveform for %s", channelLabel(boardID, chanID, rawId, sipmId).Data());

            TH1F* onehitHist = chanDir.make<TH1F>(cname, ctitle, kWaveformNBins, 0, kWaveformNBins);
            onehitHist->GetYaxis()->SetTitle("ADC Value");
            onehitHist->GetXaxis()->SetTitle("Tick");

            fillFixedWaveform(onehitHist, waveform);

            singleWaveformHistos_[chKey] = onehitHist;
            channelWaveformStored_.insert(chKey);

            // density from first-hit snapshot
            const int nbx = h_global_waveform_density_->GetNbinsX();
            const int n   = std::min<int>((int)waveform.size(), nbx);
            for(int i = 0; i < n; ++i)
              h_global_waveform_density_->Fill(i, (double)waveform[(size_t)i]);
          }
      }

    // -----------------------
    // Periodic refresh/stream
    // -----------------------
    ++eventCounter_;
    ++waveformCounter_;

    const bool doSummariesEvent = (freqDQM_ > 0) && (eventCounter_ % freqDQM_ == 0);
    if(enableDiskMaps_ && doSummariesEvent) refreshDiskMaps();

    if(!sendHists_ || !histSender_) return;

    const bool doWaveforms = (freqWaveforms_ > 0) && (waveformCounter_ % freqWaveforms_ == 0);
    if(!doSummariesEvent && !doWaveforms) return;

    std::map<std::string, std::vector<TH1*>> hists_to_send;

    if(doSummariesEvent)
      {
        hists_to_send[moduleTag_ + "/Global:replace"] = {
          h_occupancy_disk0_, h_occupancy_disk1_,
          h_baseline_disk0_,  h_baseline_disk1_,
          h_rms_disk0_,       h_rms_disk1_,
          h_maxval_disk0_,    h_maxval_disk1_,
          h_asymmetry,
          h_global_channel_dist_,
          h_global_board_dist_,
          h_global_board_vs_channel_,
          h_global_waveform_density_,
          h_waveform_size_,
          h_baseline_vs_disk
        };

        if(enableDiskMaps_)
          {
            for(auto m : modes_)
              {
                std::string groupPath = moduleTag_ + "/DiskMaps/" + modeSuffix(m) + ":replace";
                if(disk0Maps_[m]) hists_to_send[groupPath].push_back(disk0Maps_[m]);
                if(disk1Maps_[m]) hists_to_send[groupPath].push_back(disk1Maps_[m]);
              }
          }

        for(auto& [bk, hmap] : boardHistos_)
          {
            const int disk    = bk.first;
            const int boardID = bk.second;
            std::string groupPath = Form("%s/Disk%d/Board%03d:replace", moduleTag_.c_str(), disk, boardID);
            for(auto& [_, h] : hmap) hists_to_send[groupPath].push_back(h);
          }
      }

    if(doWaveforms)
      {
        for(auto& [k, hist] : channelWaveformHistos_)
          {
            std::string groupPath =
              Form("%s/Waveforms/Disk%d/Board%03d:replace", moduleTag_.c_str(), k.disk, k.board);
            hists_to_send[groupPath].push_back(hist);
          }

        for(auto& [k, hist] : singleWaveformHistos_)
          {
            std::string groupPath =
              Form("%s/OneHitWaveforms/Disk%d/Board%03d:replace", moduleTag_.c_str(), k.disk, k.board);
            hists_to_send[groupPath].push_back(hist);
          }
      }

    try
      {
        histSender_->sendHistograms(hists_to_send);
        histSendErrorCount_ = 0;
      }
    catch(const std::exception& e)
      {
        ++histSendErrorCount_;
        mf::LogError("CaloDigiDQM")
          << "HistoSender::sendHistograms exception (" << histSendErrorCount_ << "): " << e.what();
        if(histSendErrorCount_ >= kMaxSendErrors_) sendHists_ = false;
      }
    catch(...)
      {
        ++histSendErrorCount_;
        mf::LogError("CaloDigiDQM")
          << "HistoSender::sendHistograms non-std exception (" << histSendErrorCount_ << ").";
        if(histSendErrorCount_ >= kMaxSendErrors_) sendHists_ = false;
      }
  }

  // ===========================
  // endJob()
  // ===========================
  void CaloDigiDQM::endJob()
  {
    if(enableDiskMaps_) refreshDiskMaps();

    mf::LogInfo("CaloDigiDQM") << "CaloDigiDQM summary:"
                               << " events=" << eventCounter_
                               << " d0=" << nFillDisk0_
                               << " d1=" << nFillDisk1_
                               << " miss=" << nFillMiss_
                               << " sendErr=" << histSendErrorCount_
                               << " badSipmId=" << nBadSipmId_;

    struct Row { ChannelKey k; WaveformSizeStats st; };
    std::vector<Row> offenders;
    offenders.reserve(wfSizeStats_.size());

    for(const auto& [k, st] : wfSizeStats_)
      if(st.min != st.max) offenders.push_back(Row{k, st});

    std::sort(offenders.begin(), offenders.end(),
              [](const Row& a, const Row& b) {
                if(a.st.nTransitions != b.st.nTransitions) return a.st.nTransitions > b.st.nTransitions;
                const uint32_t ra = a.st.max - a.st.min;
                const uint32_t rb = b.st.max - b.st.min;
                if(ra != rb) return ra > rb;
                return a.st.nMismatchToFirst > b.st.nMismatchToFirst;
              });

    mf::LogInfo("CaloDigiDQM") << "Waveform-size summary:"
                               << " channels=" << wfSizeStats_.size()
                               << " variable=" << offenders.size()
                               << " nbins=" << kWaveformNBins;

    const size_t top = std::min<size_t>(20, offenders.size());
    if(top)
      {
        std::ostringstream os;
        os << "Top " << top << " variable-size channels:\n";
        for(size_t i = 0; i < top; ++i)
          {
            const auto& r = offenders[i];
            os << "  (D" << r.k.disk << " B" << r.k.board << " C" << r.k.chan << ")"
               << " first=" << r.st.first
               << " min=" << r.st.min
               << " max=" << r.st.max
               << " seen=" << r.st.nSeen
               << " trans=" << r.st.nTransitions
               << " mismatch=" << r.st.nMismatchToFirst
               << " pad=" << r.st.nPadded
               << " trunc=" << r.st.nTruncated << "\n";
          }
        mf::LogInfo("CaloDigiDQM") << os.str();
      }
  }

} // namespace mu2e

DEFINE_ART_MODULE(mu2e::CaloDigiDQM);
