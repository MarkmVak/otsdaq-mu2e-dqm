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

#include "TH1F.h"
#include "TH2D.h"
#include "TH2I.h"
#include "TProfile.h"
#include "TString.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <string>
#include <utility>
#include <vector>

/*
 * CaloDQMOffline
 *
 * Produces detector-aware monitoring outputs for the Mu2e calorimeter:
 *  - Global summaries per disk (occupancy, baseline, RMS, max ADC)
 *  - 2D summaries (board vs channel, waveform density)
 *  - Disk maps (per-SiPM heatmaps for Amp/Sum/Asym/Baseline/RMS)
 *  - Board-level 1D summaries and per-channel waveforms (live + first-hit snapshot)
 * Optionally streams histogram groups to otsdaq via HistoSender.
 */

namespace mu2e
{

class CaloDQMOffline : public art::EDAnalyzer
{
  public:
	struct Config
	{
		fhicl::Atom<std::string> address{fhicl::Name("address"),
		                                 "mu2edaq11-data.fnal.gov"};
		fhicl::Atom<int>         port{fhicl::Name("port"), 6000};
		fhicl::Atom<std::string> moduleTag{fhicl::Name("moduleTag"), "CaloDQM"};
		fhicl::Atom<int>         freqDQM{fhicl::Name("freqDQM"), 100};

		fhicl::Atom<std::string> caloDigiModuleLabel{fhicl::Name("caloDigiModuleLabel"),
		                                             "CaloDigi"};
		fhicl::Atom<bool> enableBoardHistos{fhicl::Name("enableBoardHistos"), true};
		fhicl::Atom<int>  maxBoardHistos{fhicl::Name("maxBoardHistos"), -1};
		fhicl::Atom<bool> enableLogging{fhicl::Name("enableLogging"), false};
		fhicl::Atom<bool> sendHists{fhicl::Name("sendHists"), false};

		fhicl::Atom<bool> enableDiskMaps{fhicl::Name("enableDiskMaps"), true};

		// diskCombines controls which disk maps are produced (["amp","baseline","rms","asym","sum"])
		fhicl::Sequence<std::string> diskCombines{fhicl::Name("diskCombines"),
		                                          std::vector<std::string>{"asym"}};

		fhicl::Atom<std::string> diskFormula{fhicl::Name("diskFormula"), ""};
	};

	explicit CaloDQMOffline(const art::EDAnalyzer::Table<Config>& config);
	void analyze(art::Event const& event) override;
	void endJob() override;

  private:
	// -----------------------
	// Map mode infrastructure
	// -----------------------
	// Amp: baseline-subtracted peak amplitude per SiPM
	// Sum: L+R amplitude (heuristic energy proxy) when both sides present
	// Asym: (L-R)/(L+R) to detect L/R gain mismatch or dead partner
	// Baseline: mean of first few samples
	// RMS: RMS of baseline window as noise proxy
	enum class MapMode
	{
		Amp,
		Sum,
		Asym,
		Baseline,
		RMS
	};

	static MapMode parseMode(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
			return std::tolower(c);
		});
		if(s == "sum")
			return MapMode::Sum;
		if(s == "asym")
			return MapMode::Asym;
		if(s == "baseline")
			return MapMode::Baseline;
		if(s == "rms")
			return MapMode::RMS;
		// default / fallback
		return MapMode::Amp;
	}

	static const char* modeSuffix(MapMode m)
	{
		switch(m)
		{
		case MapMode::Amp:
			return "Amp";
		case MapMode::Sum:
			return "Sum";
		case MapMode::Asym:
			return "Asym";
		case MapMode::Baseline:
			return "Baseline";
		case MapMode::RMS:
			return "RMS";
		}
		return "Amp";
	}

	static const char* modeFolder(MapMode m)
	{
		switch(m)
		{
		case MapMode::Amp:
			return "DiskAmp";
		case MapMode::Sum:
			return "DiskSum";
		case MapMode::Asym:
			return "DiskAsym";
		case MapMode::Baseline:
			return "DiskBaseline";
		case MapMode::RMS:
			return "DiskRMS";
		}
		return "DiskAmp";
	}

	// Configure titles/axes/ranges for the disk heatmaps
	void setDiskMapTitles(mu2e::THMu2eCaloDisk* h, int disk, MapMode mode)
	{
		if(!h)
			return;

		const char* ztitle = "Value [ADC]";
		const char* main   = "SiPM value";

		switch(mode)
		{
		case MapMode::Amp:
			main   = "SiPM amplitude at peak (baseline-subtracted)";
			ztitle = "Amplitude [ADC]";
			break;

		case MapMode::Sum:
			main   = "Crystal energy proxy: L+R amplitude";
			ztitle = "L+R [ADC]";
			break;

		case MapMode::Asym:
			main   = "SiPM asymmetry (L-R)/(L+R)";
			ztitle = "Asymmetry (L-R)/(L+R)";
			h->SetMinimum(-1.0);  // physical bounds of asymmetry
			h->SetMaximum(1.0);
			break;

		case MapMode::Baseline:
			main   = "SiPM baseline (mean of first samples)";
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

	// --------------
	// Data members
	// --------------
	std::vector<MapMode> modes_;  // enabled map modes from FHiCL

	art::InputTag caloDigiTag_;
	std::string   caloDigiModuleLabel_;
	bool          enableBoardHistos_;
	int           maxBoardHistos_;
	bool          enableLogging_;
	int           freqDQM_;
	std::string   address_;
	int           port_;
	std::string   moduleTag_;
	bool          sendHists_;

	ots::HistoSender* histSender_{nullptr};
	int               eventCounter_ = 0;

	// Event-level counters for quick health checks (filled/missed by disk)
	int nFillDisk0_{0};
	int nFillDisk1_{0};
	int nFillMiss_{0};

	// Board-level containers:
	//  - boardHistos_: per-board summary histograms keyed by (disk,boardID)
	//  - cached{Histos,Channels}Dirs_: lazily-created TFile subdirectories
	std::map<std::pair<int, int>, std::map<std::string, TH1F*>>         boardHistos_;
	std::map<std::pair<int, int>, std::unique_ptr<art::TFileDirectory>> cachedHistosDirs_;
	std::map<std::pair<int, int>, std::unique_ptr<art::TFileDirectory>>
	    cachedChannelsDirs_;

	// Per-channel waveforms:
	//  - channelWaveformHistos_: live-updating waveforms
	//  - singleWaveformHistos_: first-hit snapshot (one per channel)
	//  - channelWaveformStored_: guard set to avoid duplicating snapshots
	std::map<std::string, TH1F*> channelWaveformHistos_;
	std::map<std::string, TH1F*> singleWaveformHistos_;
	std::set<std::string>        channelWaveformStored_;

	// Temporary per-event caches to pair L/R channels and compute asymmetry
	std::map<int, float> maxValueBySiPM;
	std::map<int, int>   crystalIdBySiPM;

	bool        enableDiskMaps_;
	std::string diskFormula_;

	// Top-level TFileService directories
	std::unique_ptr<art::TFileDirectory> disk0Dir_;
	std::unique_ptr<art::TFileDirectory> disk1Dir_;
	std::unique_ptr<art::TFileDirectory> globalDir_;

	// For each map mode: make a subfolder and two THMu2eCaloDisk heatmaps (Disk 0/1)
	std::map<MapMode, art::TFileDirectory>   diskMapDirs_;
	std::map<MapMode, mu2e::THMu2eCaloDisk*> disk0Maps_;
	std::map<MapMode, mu2e::THMu2eCaloDisk*> disk1Maps_;

	// -------- Global / summary histograms --------
	// h_asymmetry: distribution of (L-R)/(L+R) across all crystals to detect L/R imbalance
	TH1F* h_asymmetry{nullptr};

	// h_baseline_vs_disk: profile of average baseline per disk (bin 1=Disk0, bin 2=Disk1)
	TProfile* h_baseline_vs_disk{nullptr};

	// h_occupancy_diskX_: per-disk occupancy vs encoded channel index (boardID*100+chanID)
	TH1F* h_occupancy_disk0_{nullptr};
	TH1F* h_occupancy_disk1_{nullptr};

	// h_baseline_diskX_: per-disk baseline vs encoded channel (filled with value as weight)
	TH1F* h_baseline_disk0_{nullptr};
	TH1F* h_baseline_disk1_{nullptr};

	// h_rms_diskX_: per-disk baseline RMS (noise proxy) vs encoded channel
	TH1F* h_rms_disk0_{nullptr};
	TH1F* h_rms_disk1_{nullptr};

	// h_maxval_diskX_: per-disk peak ADC (raw, not baseline-subtracted) vs encoded channel
	TH1F* h_maxval_disk0_{nullptr};
	TH1F* h_maxval_disk1_{nullptr};

	// Global ID distributions for quick sanity checks
	TH1F* h_global_channel_dist_{nullptr};  // counts by channel ID [0..19]
	TH1F* h_global_board_dist_{nullptr};    // counts by board ID   [0..159]

	// h_global_board_vs_channel_: 2D hit occupancy per (boardID,chanID)
	TH2I* h_global_board_vs_channel_{nullptr};

	// h_global_waveform_density_: 2D density over (tick,ADC) aggregated from first-hit snapshots
	TH2D* h_global_waveform_density_{nullptr};

	// Mapping from SiPMID to raw electronics IDs (board/chan) and disk
	mu2e::ProditionsHandle<mu2e::CaloDAQMap> _calodaqconds_h;
};

// ===========================
// Constructor
// ===========================
CaloDQMOffline::CaloDQMOffline(const art::EDAnalyzer::Table<Config>& config)
    : EDAnalyzer{config}
    , caloDigiTag_{config().caloDigiModuleLabel()}
    , caloDigiModuleLabel_(config().caloDigiModuleLabel())
    , enableBoardHistos_(config().enableBoardHistos())
    , maxBoardHistos_(config().maxBoardHistos())
    , enableLogging_(config().enableLogging())
    , freqDQM_(config().freqDQM())
    , address_(config().address())
    , port_(config().port())
    , moduleTag_(config().moduleTag())
    , sendHists_(config().sendHists())
    , enableDiskMaps_(config().enableDiskMaps())
    , diskFormula_(config().diskFormula())
{
	// Parse enabled disk map modes from FHiCL (defaults to {"asym"} if empty)
	std::vector<std::string> rawModes = config().diskCombines();
	if(rawModes.empty())
		rawModes = {"asym"};  // fallback

	modes_.reserve(rawModes.size());
	for(auto& s : rawModes)
	{
		modes_.push_back(parseMode(s));
	}

	// Initialize sender if streaming is enabled
	if(sendHists_)
	{
		histSender_ = new ots::HistoSender(address_, port_);
	}

	// Create top-level directories: Disk0, Disk1, Global_Histograms
	art::ServiceHandle<art::TFileService> tfs;
	disk0Dir_  = std::make_unique<art::TFileDirectory>(tfs->mkdir("Disk0"));
	disk1Dir_  = std::make_unique<art::TFileDirectory>(tfs->mkdir("Disk1"));
	globalDir_ = std::make_unique<art::TFileDirectory>(tfs->mkdir("Global_Histograms"));

	// --- Global histograms ---

	// Occupancy per encoded channel (x = boardID*100 + chanID). Y counts hits.
	h_occupancy_disk0_ =
	    disk0Dir_->make<TH1F>("h_occ_d0", "Occupancy (Disk 0)", 8020, 0, 8020);
	h_occupancy_disk0_->GetXaxis()->SetTitle("Encoded Channel (boardID * 100 + chanID)");
	h_occupancy_disk0_->GetYaxis()->SetTitle("Hit Count");

	h_occupancy_disk1_ =
	    disk1Dir_->make<TH1F>("h_occ_d1", "Occupancy (Disk 1)", 7920, 8000, 15920);
	h_occupancy_disk1_->GetXaxis()->SetTitle("Encoded Channel (boardID * 100 + chanID)");
	h_occupancy_disk1_->GetYaxis()->SetTitle("Hit Count");

	// 2D occupancy index: good for spotting missing rows/columns or swapped cabling
	h_global_board_vs_channel_ = globalDir_->make<TH2I>(
	    "h_board_vs_channel", "Board vs Channel", 160, 0, 160, 20, 0, 20);
	h_global_board_vs_channel_->GetXaxis()->SetTitle("Board ID");
	h_global_board_vs_channel_->GetYaxis()->SetTitle("Channel ID");

	// Aggregated waveform shape: x=tick, y=ADC; highlights global saturation, clipping, or drift
	h_global_waveform_density_ = globalDir_->make<TH2D>(
	    "h_waveform_density", "Waveform Density", 150, 0, 150, 400, 2000, 4095);
	h_global_waveform_density_->GetXaxis()->SetTitle("Tick");
	h_global_waveform_density_->GetYaxis()->SetTitle("ADC Value");

	// Baseline and noise (RMS) per encoded channel; used to detect hot/noisy channels
	h_baseline_disk0_ =
	    disk0Dir_->make<TH1F>("h_base_d0", "Baseline (Disk 0)", 8020, 0, 8020);
	h_baseline_disk0_->SetMarkerStyle(20);
	h_baseline_disk0_->GetXaxis()->SetTitle("Encoded Channel (boardID * 100 + chanID)");
	h_baseline_disk0_->GetYaxis()->SetTitle("Baseline");

	h_baseline_disk1_ =
	    disk1Dir_->make<TH1F>("h_base_d1", "Baseline (Disk 1)", 7920, 8000, 15920);
	h_baseline_disk1_->SetMarkerStyle(20);
	h_baseline_disk1_->GetXaxis()->SetTitle("Encoded Channel (boardID * 100 + chanID)");
	h_baseline_disk1_->GetYaxis()->SetTitle("Baseline");

	h_rms_disk0_ = disk0Dir_->make<TH1F>("h_rms_d0", "RMS (Disk 0)", 8020, 0, 8020);
	h_rms_disk0_->SetMarkerStyle(20);
	h_rms_disk0_->GetXaxis()->SetTitle("Encoded Channel (boardID * 100 + chanID)");
	h_rms_disk0_->GetYaxis()->SetTitle("Baseline RMS");

	h_rms_disk1_ = disk1Dir_->make<TH1F>("h_rms_d1", "RMS (Disk 1)", 7920, 8000, 15920);
	h_rms_disk1_->SetMarkerStyle(20);
	h_rms_disk1_->GetXaxis()->SetTitle("Encoded Channel (boardID * 100 + chanID)");
	h_rms_disk1_->GetYaxis()->SetTitle("Baseline RMS");

	// Peak ADC per encoded channel (raw). Useful to catch saturation and dead channels.
	h_maxval_disk0_ =
	    disk0Dir_->make<TH1F>("h_max_d0", "Max ADC (Disk 0)", 8020, 0, 8020);
	h_maxval_disk0_->SetMarkerStyle(20);
	h_maxval_disk0_->GetXaxis()->SetTitle("Encoded Channel (boardID * 100 + chanID)");
	h_maxval_disk0_->GetYaxis()->SetTitle("Maximum ADC Value");

	h_maxval_disk1_ =
	    disk1Dir_->make<TH1F>("h_max_d1", "Max ADC (Disk 1)", 7920, 8000, 15920);
	h_maxval_disk1_->SetMarkerStyle(20);
	h_maxval_disk1_->GetXaxis()->SetTitle("Encoded Channel (boardID * 100 + chanID)");
	h_maxval_disk1_->GetYaxis()->SetTitle("Maximum ADC Value");

	// Disk-level average baseline; shows offsets/mode changes between disks
	h_baseline_vs_disk =
	    globalDir_->make<TProfile>("h_base_vs_d", "Baseline vs Disk", 2, 0, 2);
	h_baseline_vs_disk->GetXaxis()->SetBinLabel(1, "Disk 0");
	h_baseline_vs_disk->GetXaxis()->SetBinLabel(2, "Disk 1");
	h_baseline_vs_disk->GetYaxis()->SetTitle("Mean Baseline [ADC]");

	// Global LR asymmetry; centered near 0 ideally. Tails signal imbalance or dead partner.
	h_asymmetry =
	    globalDir_->make<TH1F>("h_asym", "Left-Right Asymmetry", 100, -1.0, 1.0);
	h_asymmetry->GetXaxis()->SetTitle("(L - R)/(L + R)");
	h_asymmetry->GetYaxis()->SetTitle("Frequency");

	// Quick integrity checks for ID distributions
	h_global_channel_dist_ = globalDir_->make<TH1F>(
	    "h_channel_dist", "Global Channel Distribution", 20, 0, 20);
	h_global_channel_dist_->GetXaxis()->SetTitle("Channel ID");
	h_global_channel_dist_->GetYaxis()->SetTitle("Frequency");

	h_global_board_dist_ =
	    globalDir_->make<TH1F>("h_board_dist", "Global Board Distribution", 160, 0, 160);
	h_global_board_dist_->GetXaxis()->SetTitle("Board ID");
	h_global_board_dist_->GetYaxis()->SetTitle("Frequency");

	// --- Disk maps per mode, grouped into subfolders like DiskAmp, DiskAsym, ...
	if(enableDiskMaps_)
	{
		for(auto m : modes_)
		{
			const char* suf    = modeSuffix(m);
			const char* folder = modeFolder(m);

			auto& modeDir =
			    diskMapDirs_.try_emplace(m, globalDir_->mkdir(folder)).first->second;

			std::string key0 = Form("disk0_%s", suf);
			std::string key1 = Form("disk1_%s", suf);

			std::string title0 = Form("Disk 0 - %s", suf);
			std::string title1 = Form("Disk 1 - %s", suf);

			// THMu2eCaloDisk: calorimeter layout heatmap; FillOffline(sipmId,value) sets z-content
			auto* d0 = modeDir.makeAndRegister<mu2e::THMu2eCaloDisk>(
			    key0.c_str(),
			    title0.c_str(),  // key, TFile title
			    key0.c_str(),
			    title0.c_str(),  // in-object name/title
			    0);              // disk index

			auto* d1 = modeDir.makeAndRegister<mu2e::THMu2eCaloDisk>(
			    key1.c_str(), title1.c_str(), key1.c_str(), title1.c_str(), 1);

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
void CaloDQMOffline::analyze(art::Event const& event)
{
	// Reset per-event pairing caches
	maxValueBySiPM.clear();
	crystalIdBySiPM.clear();

	mf::LogInfo("CaloDQMOffline") << "CaloDQMOffline is running.";

	// Input collection of SiPM waveforms
	const auto& caloDigis = *event.getValidHandle<CaloDigiCollection>(caloDigiTag_);

	// Electronics mapping (SiPMID -> rawId -> board/channel, also implies disk)
	const auto& calodaqconds = _calodaqconds_h.get(event.id());

	for(const auto& digi : caloDigis)
	{
		const auto& waveform = digi.waveform();
		if(waveform.size() < 5 || digi.peakpos() >= (int)waveform.size())
			continue;

		// Basic features from a fixed "baseline" window at the start of the waveform
		float baseline =
		    std::accumulate(waveform.begin(), waveform.begin() + 5, 0.0f) / 5.0f;

		float mean_sq =
		    std::inner_product(
		        waveform.begin(), waveform.begin() + 5, waveform.begin(), 0.0f) /
		    5.0f;

		float rms = (mean_sq > baseline * baseline)
		                ? std::sqrt(mean_sq - baseline * baseline)
		                : 0.0f;

		if(!std::isfinite(baseline) || !std::isfinite(rms))
			continue;

		// SiPM and crystal pairing info to compute L/R quantities
		int sipmId              = digi.SiPMID();
		int crystalId           = sipmId / 2;
		crystalIdBySiPM[sipmId] = crystalId;

		const float ampRaw =
		    waveform[digi.peakpos()];  // raw peak (not baseline-subtracted)
		maxValueBySiPM[sipmId] = ampRaw;

		// partnerSiPM: even->odd, odd->even (two SiPMs per crystal)
		const int partnerSiPM = (sipmId % 2 == 0) ? (sipmId + 1) : (sipmId - 1);

		double L = 0.0, R = 0.0;
		bool   haveLR = false;

		auto itP = maxValueBySiPM.find(partnerSiPM);
		auto itC = crystalIdBySiPM.find(partnerSiPM);
		if(itP != maxValueBySiPM.end() && itC != crystalIdBySiPM.end() &&
		   crystalIdBySiPM[sipmId] == itC->second)
		{
			// Convention: even index is "L", odd is "R"
			if(sipmId % 2 == 0)
			{
				L = maxValueBySiPM[sipmId];
				R = itP->second;
			}
			else
			{
				L = itP->second;
				R = maxValueBySiPM[sipmId];
			}
			haveLR = true;
		}

		// Fill global asymmetry once a valid LR pair is seen
		if(haveLR)
		{
			const double denom = L + R;
			const double asym  = (denom > 0.0) ? (L - R) / denom : 0.0;
			h_asymmetry->Fill(asym);
		}

		// SiPM -> rawID -> board/chan/disk; rawId==9999 indicates missing mapping
		int rawId = calodaqconds.rawId(mu2e::CaloSiPMId(sipmId)).id();
		if(rawId == 9999)
			continue;

		int boardID = rawId / 20;    // 20 channels per board
		int chanID  = rawId % 20;    // channel within board
		int disk    = boardID / 80;  // 80 boards per disk => 0 or 1

		// Encoded 1D index used for per-disk 1D histograms
		int encoded = boardID * 100 + chanID;

		// Physics values for map modes
		const double amp = ampRaw - baseline;  // baseline-subtracted peak
		const double sumLR =
		    haveLR ? (L + R) : ampRaw;  // fallback to raw peak if partner missing
		const double asymLR  = (haveLR && (L + R) > 0.0) ? (L - R) / (L + R) : 0.0;
		const double baseVal = baseline;
		const double rmsVal  = rms;

		// Fill disk heatmaps (per-SiPM value) for enabled modes
		if(enableDiskMaps_)
		{
			for(auto m : modes_)
			{
				double val = 0.0;
				switch(m)
				{
				case MapMode::Amp:
					val = amp;
					break;
				case MapMode::Sum:
					val = sumLR;
					break;
				case MapMode::Asym:
					val = asymLR;
					break;
				case MapMode::Baseline:
					val = baseVal;
					break;
				case MapMode::RMS:
					val = rmsVal;
					break;
				}

				if(disk == 0 && disk0Maps_[m])
				{
					disk0Maps_[m]->FillOffline(sipmId, val);
				}
				else if(disk == 1 && disk1Maps_[m])
				{
					disk1Maps_[m]->FillOffline(sipmId, val);
				}
			}
		}

		// Bookkeeping counters
		if(disk == 0)
			++nFillDisk0_;
		else if(disk == 1)
			++nFillDisk1_;
		else
			++nFillMiss_;

		// Global distributions for quick sanity checks
		h_global_board_dist_->Fill(boardID);
		h_global_channel_dist_->Fill(chanID);

		// Per-disk 1D summaries vs encoded channel
		(disk == 0 ? h_occupancy_disk0_ : h_occupancy_disk1_)->Fill(encoded);
		(disk == 0 ? h_baseline_disk0_ : h_baseline_disk1_)->Fill(encoded, baseline);
		(disk == 0 ? h_rms_disk0_ : h_rms_disk1_)->Fill(encoded, rms);
		(disk == 0 ? h_maxval_disk0_ : h_maxval_disk1_)->Fill(encoded, ampRaw);

		// Disk-level baseline profile (bin centers at 0.5/1.5 encode disk index)
		h_baseline_vs_disk->Fill(disk == 0 ? 0.5 : 1.5, baseline);

		// ---------------------------
		// Board-level / channel-level
		// ---------------------------
		if(enableBoardHistos_ &&
		   (maxBoardHistos_ < 0 || (int)boardHistos_.size() < maxBoardHistos_))
		{
			std::pair<int, int> boardKey = std::make_pair(disk, boardID);

			// Create per-board folders on first encounter:
			//   DiskX/Board_YYY/Histograms for 1D summaries
			//   DiskX/Board_YYY/Channels   for per-channel waveforms (first-hit snapshots)
			if(cachedHistosDirs_.find(boardKey) == cachedHistosDirs_.end())
			{
				art::TFileDirectory boardDir =
				    (disk == 0 ? *disk0Dir_ : *disk1Dir_)
				        .mkdir("Board_" + std::to_string(boardID));

				cachedHistosDirs_[boardKey] =
				    std::make_unique<art::TFileDirectory>(boardDir.mkdir("Histograms"));

				cachedChannelsDirs_[boardKey] =
				    std::make_unique<art::TFileDirectory>(boardDir.mkdir("Channels"));
			}

			art::TFileDirectory& histosDir = *cachedHistosDirs_[boardKey];
			auto&                histos    = boardHistos_[boardKey];

			if(histos.empty())
			{
				// Occupancy per channel (0..19)
				histos["occ"] =
				    histosDir.make<TH1F>(Form("D%d_B%03d_Occupancy", disk, boardID),
				                         Form("Occupancy for D%d B%03d", disk, boardID),
				                         20,
				                         0,
				                         20);
				histos["occ"]->GetXaxis()->SetTitle("Channel ID");
				histos["occ"]->GetYaxis()->SetTitle("Count");

				// Baseline distribution per channel (filled with value as weight)
				histos["base"] =
				    histosDir.make<TH1F>(Form("D%d_B%03d_Baseline", disk, boardID),
				                         Form("Baseline for D%d B%03d", disk, boardID),
				                         20,
				                         0,
				                         20);
				histos["base"]->GetXaxis()->SetTitle("Channel ID");
				histos["base"]->GetYaxis()->SetTitle("Count");
				histos["base"]->SetMarkerStyle(20);

				// Baseline RMS per channel (noise proxy)
				histos["rms"] =
				    histosDir.make<TH1F>(Form("D%d_B%03d_RMS", disk, boardID),
				                         Form("RMS for D%d B%03d", disk, boardID),
				                         20,
				                         0,
				                         20);
				histos["rms"]->GetXaxis()->SetTitle("Channel ID");
				histos["rms"]->GetYaxis()->SetTitle("ADC RMS");
				histos["rms"]->SetMarkerStyle(20);

				// Peak ADC per channel (raw)
				histos["max"] =
				    histosDir.make<TH1F>(Form("D%d_B%03d_Max", disk, boardID),
				                         Form("Max for D%d B%03d", disk, boardID),
				                         20,
				                         0,
				                         20);
				histos["max"]->GetXaxis()->SetTitle("Channel ID");
				histos["max"]->GetYaxis()->SetTitle("Max ADC");
				histos["max"]->SetMarkerStyle(20);
			}

			// Fill board-level summaries
			histos["occ"]->Fill(chanID);
			histos["base"]->Fill(chanID, baseline);
			histos["rms"]->Fill(chanID, rms);
			histos["max"]->Fill(chanID, ampRaw);

			// Per-channel waveform histograms
			//   - Live waveform: updated every time channel is seen
			//   - Snapshot (FirstHit): created only the first time we see the channel
			std::string wf_key = Form("D%d_B%03d_C%02d", disk, boardID, chanID);

			if(!channelWaveformHistos_.count(wf_key))
			{
				TString cname = Form("%s_Waveform", wf_key.c_str());  // histogram key
				TString ctitle =
				    Form("D%d B%03d C%02d - Live Waveform", disk, boardID, chanID);

				channelWaveformHistos_[wf_key] = histosDir.make<TH1F>(
				    cname, ctitle, waveform.size(), 0, waveform.size());
				channelWaveformHistos_[wf_key]->GetYaxis()->SetTitle("ADC Value");
				channelWaveformHistos_[wf_key]->GetXaxis()->SetTitle("Tick");
			}

			TH1F* chanHist = channelWaveformHistos_[wf_key];
			for(size_t i = 0; i < waveform.size(); ++i)
			{
				chanHist->SetBinContent(i + 1, waveform[i]);
			}

			// Create one-time snapshot in Channels/ on first encounter of this channel
			if(channelWaveformStored_.count(wf_key) == 0)
			{
				art::TFileDirectory& chanDir = *cachedChannelsDirs_[boardKey];

				TString cname  = Form("%s_FirstHit", wf_key.c_str());
				TString ctitle = Form("D%d B%03d C%02d - Snapshot Waveform (first hit)",
				                      disk,
				                      boardID,
				                      chanID);

				TH1F* onehitHist = chanDir.make<TH1F>(
				    cname, ctitle, waveform.size(), 0, waveform.size());
				onehitHist->GetYaxis()->SetTitle("ADC Value");
				onehitHist->GetXaxis()->SetTitle("Tick");

				for(size_t i = 0; i < waveform.size(); ++i)
				{
					onehitHist->SetBinContent(i + 1, waveform[i]);
				}

				singleWaveformHistos_[wf_key] = onehitHist;
				channelWaveformStored_.insert(wf_key);

				// Contribute to global 2D summaries once per channel
				h_global_board_vs_channel_->Fill(boardID, chanID);
				for(size_t i = 0; i < waveform.size(); ++i)
				{
					h_global_waveform_density_->Fill(i, waveform[i]);
				}
			}
		}  // end board/channel block
	}      // end loop over digis

	// Throttle streaming to every freqDQM events
	++eventCounter_;
	if(eventCounter_ % freqDQM_ != 0)
		return;

	// ---------------------------
	// Streaming to otsdaq
	// ---------------------------
	// hists_to_send groups are keyed by a path with an optional ":replace" suffix
	// to instruct the receiver to atomically replace the group's contents.
	std::map<std::string, std::vector<TH1*>> hists_to_send;

	// Global group (always sent): cross-disk summaries and integrity plots
	hists_to_send[moduleTag_ + "/Global:replace"] = {h_occupancy_disk0_,
	                                                 h_occupancy_disk1_,
	                                                 h_baseline_disk0_,
	                                                 h_baseline_disk1_,
	                                                 h_rms_disk0_,
	                                                 h_rms_disk1_,
	                                                 h_maxval_disk0_,
	                                                 h_maxval_disk1_,
	                                                 h_asymmetry,
	                                                 h_global_channel_dist_,
	                                                 h_global_board_dist_,
	                                                 h_global_board_vs_channel_,
	                                                 h_global_waveform_density_,
	                                                 h_baseline_vs_disk};

	// Disk map groups, one subpath per mode (e.g., Module/DiskMaps/Amp)
	if(sendHists_ && enableDiskMaps_)
	{
		for(auto m : modes_)
		{
			const char* suf       = modeSuffix(m);
			std::string groupPath = moduleTag_ + "/DiskMaps/" + suf + ":replace";

			if(disk0Maps_[m])
				hists_to_send[groupPath].push_back(disk0Maps_[m]);
			if(disk1Maps_[m])
				hists_to_send[groupPath].push_back(disk1Maps_[m]);
		}
	}

	// Live waveforms grouped by disk and board
	for(auto& [key, hist] : channelWaveformHistos_)
	{
		int disk, boardID, chanID;
		sscanf(key.c_str(), "D%d_B%03d_C%02d", &disk, &boardID, &chanID);
		std::string groupPath = Form(
		    "%s/Waveforms/Disk%d/Board%03d:replace", moduleTag_.c_str(), disk, boardID);
		hists_to_send[groupPath].push_back(hist);
	}

	// One-hit snapshot waveforms grouped similarly
	for(auto& [key, hist] : singleWaveformHistos_)
	{
		int disk, boardID, chanID;
		sscanf(key.c_str(), "D%d_B%03d_C%02d", &disk, &boardID, &chanID);
		std::string groupPath = Form("%s/OneHitWaveforms/Disk%d/Board%03d:replace",
		                             moduleTag_.c_str(),
		                             disk,
		                             boardID);
		hists_to_send[groupPath].push_back(hist);
	}

	// Board summaries (occ/base/rms/max) grouped by disk/board
	for(auto& [boardKey, hmap] : boardHistos_)
	{
		int         disk    = boardKey.first;
		int         boardID = boardKey.second;
		std::string groupPath =
		    Form("%s/Disk%d/Board%03d:replace", moduleTag_.c_str(), disk, boardID);
		for(auto& [_, h] : hmap)
		{
			hists_to_send[groupPath].push_back(h);
		}
	}

	if(sendHists_ && histSender_)
	{
		histSender_->sendHistograms(hists_to_send);
		std::cout << "Sending " << hists_to_send.size() << " histogram groups"
		          << std::endl;
		for(const auto& [dir, vec] : hists_to_send)
		{
			std::cout << "Group: " << dir << " - " << vec.size() << " hists" << std::endl;
		}
	}
}

// ===========================
// endJob()
// ===========================
void CaloDQMOffline::endJob() {}

}  // namespace mu2e

DEFINE_ART_MODULE(mu2e::CaloDQMOffline);
