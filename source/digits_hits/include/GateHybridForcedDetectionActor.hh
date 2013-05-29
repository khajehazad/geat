/*----------------------
  GATE version name: gate_v6

  Copyright (C): OpenGATE Collaboration

  This software is distributed under the terms
  of the GNU Lesser General  Public Licence (LGPL)
  See GATE/LICENSE.txt for further details
  ----------------------*/

//-----------------------------------------------------------------------------
/// \class GateHybridForcedDetectionActor
//-----------------------------------------------------------------------------

#include "GateConfiguration.h"
#ifdef GATE_USE_RTK

#ifndef GATEHYBRIDFORCEDDECTECTIONACTOR_HH
#define GATEHYBRIDFORCEDDECTECTIONACTOR_HH

#include "globals.hh"
#include "G4String.hh"
#include <iomanip>   
#include <vector>

// Gate 
#include "GateVActor.hh"
#include "GateHybridForcedDetectionActorMessenger.hh"
#include "GateImage.hh"
#include "GateSourceMgr.hh"
#include "GateVImageVolume.hh"

// Geant4
#include <G4VEMDataSet.hh>
#include <G4EmCalculator.hh>
#include <G4VDataSetAlgorithm.hh>
#include <G4LivermoreComptonModel.hh>
#include <G4LogLogInterpolation.hh>
#include <G4CompositeEMDataSet.hh>
#include <G4CrossSectionHandler.hh>

// itk
#include <itkImageFileWriter.h>
#include <itkBinaryFunctorImageFilter.h>
#include <itkTimeProbe.h>
#include <itkLinearInterpolateImageFunction.h>

// rtk
#include <rtkConstantImageSource.h>
#include <rtkReg23ProjectionGeometry.h>
#include <rtkJosephForwardProjectionImageFilter.h>

//-----------------------------------------------------------------------------
// Handling of the interpolation weight in primary: store the weights and
// the material indices in vectors and return nada. The integral is computed in the
// ProjectedValueAccumulation since one has to repeat the same ray cast for each
// and every energy of the primary.
class InterpolationWeightMultiplication
{
public:
  typedef itk::Vector<double, 3> VectorType;

  InterpolationWeightMultiplication() {};
  ~InterpolationWeightMultiplication() {};
  bool operator!=( const InterpolationWeightMultiplication & ) const {
    return false;
  }
  bool operator==(const InterpolationWeightMultiplication & other) const {
    return !( *this != other );
  }

  inline double operator()( const rtk::ThreadIdType threadId,
                            const double stepLengthInVoxel,
                            const double weight,
                            const float *p,
                            const int i) {
    m_InterpolationWeights[threadId][(int)(p[i])] += stepLengthInVoxel * weight;
    return 0.;
  }

  std::vector<double>* GetInterpolationWeights() { return m_InterpolationWeights; }

private:
  std::vector<double> m_InterpolationWeights[ITK_MAX_THREADS];
};
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Mother class for accumulation. Purely virtual (FIXME).
class VAccumulation
{
public:
  static const unsigned int Dimension = 3;
  typedef itk::Vector<double, Dimension>                             VectorType;
  typedef float                                                      InputPixelType;
  typedef itk::Image<InputPixelType, Dimension>                      InputImageType;
  typedef itk::Image<double, 2>                                      MaterialMuImageType;

  bool operator!=( const VAccumulation & ) const
  {
    return false;
  }

  bool operator==(const VAccumulation & other) const
  {
    return !( *this != other );
  }
  void SetVolumeSpacing(const VectorType &_arg){ m_VolumeSpacing = _arg; }
  void SetInterpolationWeights(std::vector<double> *_arg){ m_InterpolationWeights = _arg; }
  void SetEnergyWeightList(std::vector<double> *_arg) { m_EnergyWeightList = _arg; }
  void Init(unsigned int nthreads) {
    for(unsigned int i=0; i<nthreads; i++) {
      m_InterpolationWeights[i].resize(m_MaterialMu->GetLargestPossibleRegion().GetSize()[0]);
      std::fill(m_InterpolationWeights[i].begin(), m_InterpolationWeights[i].end(), 0.);
    }
  }

  // Solid angle from the source to pixel vector in voxels
  void SetSolidAngleParameters(const InputImageType::Pointer proj,
                               const VectorType &u,
                               const VectorType &v) {
    m_DetectorOrientationTimesPixelSurface = proj->GetSpacing()[0] *
                                             proj->GetSpacing()[1] *
                                             itk::CrossProduct(u,v);
  }
  double GetSolidAngle(const VectorType &sourceToPixelInVox) const {
    VectorType sourceToPixelInMM;
    for(int i=0; i<3; i++)
      sourceToPixelInMM[i] =  sourceToPixelInVox[i]*m_VolumeSpacing[i];
    return std::abs(sourceToPixelInMM * m_DetectorOrientationTimesPixelSurface / pow(sourceToPixelInMM.GetNorm(), 3.));
  }

  MaterialMuImageType *GetMaterialMu() { return m_MaterialMu.GetPointer(); }

  void CreateMaterialMuMap(G4EmCalculator *emCalculator,
                           const double energySpacing,
                           const double energyMax,
                           GateVImageVolume * gate_image_volume) {
    std::vector<double> energyList;
    energyList.push_back(0.);
    while(energyList.back()<energyMax)
      energyList.push_back(energyList.back()+energySpacing);
    CreateMaterialMuMap(emCalculator, energyList, gate_image_volume);
    MaterialMuImageType::SpacingType spacing;
    spacing[0] = 1.;
    spacing[1] = energySpacing;
    m_MaterialMu->SetSpacing(spacing);
    MaterialMuImageType::PointType origin;
    origin.Fill(0.);
    m_MaterialMu->SetOrigin(origin);
  }

  void CreateMaterialMuMap(G4EmCalculator *emCalculator,
                           const std::vector<double> &Elist,
                           GateVImageVolume * gate_image_volume) {
    itk::TimeProbe muProbe;
    muProbe.Start();

    // Get image materials + world
    std::vector<G4Material*> m;
    gate_image_volume->BuildLabelToG4MaterialVector(m);
    GateVVolume *v = gate_image_volume;
    while (v->GetLogicalVolumeName() != "world_log")
      v = v->GetParentVolume();
    m.push_back(const_cast<G4Material*>(v->GetMaterial()));

    // Get the list of involved processes (Rayleigh, Compton, PhotoElectric)
    G4ParticleDefinition* particle = G4ParticleTable::GetParticleTable()->FindParticle("gamma");
    G4ProcessVector* plist = particle->GetProcessManager()->GetProcessList();
    std::vector<G4String> processNameVector;
    for (G4int j = 0; j < plist->size(); j++) {
      G4ProcessType type = (*plist)[j]->GetProcessType();
      std::string name = (*plist)[j]->GetProcessName();
      if ((type == fElectromagnetic) && (name != "msc")) {
        processNameVector.push_back(name);
      }
    }

    MaterialMuImageType::RegionType region;
    region.SetSize(0, m.size());
    region.SetSize(1, Elist.size());
    m_MaterialMu = MaterialMuImageType::New();
    m_MaterialMu->SetRegions(region);
    m_MaterialMu->Allocate();
    itk::ImageRegionIterator< MaterialMuImageType > it(m_MaterialMu, region);
    for(unsigned int e=0; e<Elist.size(); e++)
      for(unsigned int i=0; i<m.size(); i++) {
        G4Material * mat = m[i];
        //double d = mat->GetDensity(); // not needed
        double mu = 0;
        for (unsigned int j = 0; j < processNameVector.size(); j++) {
          // Note: the G4EmCalculator retrive the correct G4VProcess
          // (standard, Penelope, Livermore) from the processName.
          double xs =
              emCalculator->ComputeCrossSectionPerVolume(Elist[e], "gamma", processNameVector[j], mat->GetName());
          // In (length unit)^{-1} according to
          // http://www.lcsim.org/software/geant4/doxygen/html/classG4EmCalculator.html#a870d5fffaca35f6e2946da432034bd4c
          mu += xs;
        }
        it.Set(mu);
        ++it;
      }

    muProbe.Stop();
    G4cout << "Computation of the mu lookup table took "
           << muProbe.GetTotal()
           << ' '
           << muProbe.GetUnit()
           << G4endl;
  }

protected:
  VectorType                                m_VolumeSpacing;
  std::vector<double>                      *m_InterpolationWeights;
  std::vector<double>                      *m_EnergyWeightList;
  MaterialMuImageType::Pointer              m_MaterialMu;
  VectorType                                m_DetectorOrientationTimesPixelSurface;
};
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Most of the computation for the primary is done in this functor. After a ray
// has been cast, it loops over the energies, computes the ray line integral for
// that energy and takes the exponential of the opposite and add.
class PrimaryValueAccumulation:
    public VAccumulation
{
public:

  PrimaryValueAccumulation() {}
  ~PrimaryValueAccumulation() {}

  inline double operator()( const rtk::ThreadIdType threadId,
                            double input,
                            const double &itkNotUsed(rayCastValue),
                            const VectorType &stepInMM,
                            const VectorType &itkNotUsed(source),
                            const VectorType &sourceToPixel,
                            const VectorType &nearestPoint,
                            const VectorType &farthestPoint) const
  {
    double *p = m_MaterialMu->GetPixelContainer()->GetBufferPointer();

    // Multiply interpolation weights by step norm in MM to convert voxel
    // intersection length to MM.
    const double stepInMMNorm = stepInMM.GetNorm();
    for(unsigned int j=0; j<m_InterpolationWeights[threadId].size()-1; j++)
      m_InterpolationWeights[threadId][j] *= stepInMMNorm;

    // The last material is the world material. One must fill the weight with
    // the length from source to nearest point and farthest point to pixel
    // point.
    VectorType worldVector = sourceToPixel - nearestPoint + farthestPoint;
    for(int i=0; i<3; i++)
      worldVector[i] *= m_VolumeSpacing[i];
    m_InterpolationWeights[threadId].back() = worldVector.GetNorm();

    // Loops over energy, multiply weights by mu, accumulate using Beer Lambert
    for(unsigned int i=0; i<m_EnergyWeightList->size(); i++) {
      double rayIntegral = 0.;
      for(unsigned int j=0; j<m_InterpolationWeights[threadId].size(); j++){
        rayIntegral += m_InterpolationWeights[threadId][j] * *p++;
      }
      input += vcl_exp(-rayIntegral) * (*m_EnergyWeightList)[i];
    }
    // FIXME: the source is not punctual but it is homogeneous on the detection plane
    // so one should not multiply by the solid angle but by the pixel surface.
    // To be discussed...
    //input *= GetSolidAngle(sourceToPixel);

    // Reset weights for next ray in thread.
    std::fill(m_InterpolationWeights[threadId].begin(), m_InterpolationWeights[threadId].end(), 0.);
    return input;
  }
};
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
class ComptonValueAccumulation:
    public VAccumulation
{
public:

  ComptonValueAccumulation() {
    // G4 data
    G4VDataSetAlgorithm* scatterInterpolation = new G4LogLogInterpolation;
    G4String scatterFile = "comp/ce-sf-";
    m_ScatterFunctionData = new G4CompositeEMDataSet( scatterInterpolation, 1., 1.);
    m_ScatterFunctionData->LoadData(scatterFile);

    m_CrossSectionHandler = new G4CrossSectionHandler;
    G4String crossSectionFile = "comp/ce-cs-";
    m_CrossSectionHandler->LoadData(crossSectionFile);
    }
  ~ComptonValueAccumulation() {
    delete m_ScatterFunctionData;
    delete m_CrossSectionHandler;
  }

  inline double operator()( const rtk::ThreadIdType threadId,
                            double input,
                            const double &itkNotUsed(rayCastValue),
                            const VectorType &stepInMM,
                            const VectorType &source,
                            const VectorType &sourceToPixel,
                            const VectorType &nearestPoint,
                            const VectorType &farthestPoint) const
  {
    // Compute ray length in world material
    // This is used to compute the length in world as well as the direction
    // of the ray in mm.
    VectorType worldVector = sourceToPixel - 2.*source + nearestPoint + farthestPoint;
    for(int i=0; i<3; i++)
      worldVector[i] *= m_VolumeSpacing[i];
    const double worldVectorNorm = worldVector.GetNorm();

    // This is taken from G4LivermoreComptonModel.cc
    double cosT = worldVector * m_Direction / worldVectorNorm;
    double x = std::sqrt(1.-cosT) * m_InvWlPhoton;// 1-cosT=2*sin(T/2)^2
    double scatteringFunction = m_ScatterFunctionData->FindValue(x,m_Z-1);

    // This is taken from GateDiffCrossSectionActor.cc and simplified
    double Eratio = 1./(1.+m_E0m*(1.-cosT));
    //double DCSKleinNishina = m_eRadiusOverCrossSectionTerm *
    //                         Eratio * Eratio *                      // DCSKleinNishinaTerm1
    //                         (Eratio + 1./Eratio - 1. + cosT*cosT); // DCSKleinNishinaTerm2
    double DCSKleinNishina = m_eRadiusOverCrossSectionTerm*Eratio*(1.+Eratio*(Eratio-1.+cosT*cosT));
    double DCScompton = DCSKleinNishina * scatteringFunction;

    // Multiply interpolation weights by step norm in MM to convert voxel
    // intersection length to MM.
    const double stepInMMNorm = stepInMM.GetNorm();
    for(unsigned int j=0; j<m_InterpolationWeights[threadId].size()-1; j++)
      m_InterpolationWeights[threadId][j] *= stepInMMNorm;

    // The last material is the world material. One must fill the weight with
    // the length from farthest point to pixel point.
    m_InterpolationWeights[threadId].back() = worldVectorNorm;

    // Pointer to adequate mus
    unsigned int e = itk::Math::Round<double, double>(Eratio*m_Energy / m_MaterialMu->GetSpacing()[1]);
    double *p = m_MaterialMu->GetPixelContainer()->GetBufferPointer() +
                e * m_MaterialMu->GetLargestPossibleRegion().GetSize()[0];

    // Ray integral
    double rayIntegral = 0.;
    for(unsigned int j=0; j<m_InterpolationWeights[threadId].size(); j++)
      rayIntegral += m_InterpolationWeights[threadId][j] * *p++;

    // Final computation
    input += vcl_exp(-rayIntegral) * DCScompton * GetSolidAngle(sourceToPixel);

    // Reset weights for next ray in thread.
    std::fill(m_InterpolationWeights[threadId].begin(), m_InterpolationWeights[threadId].end(), 0.);
    return input;
  }

  void SetDirection(const VectorType &_arg){ m_Direction = _arg; }

  void SetEnergyAndZ(const double  &energy, const unsigned int &Z, const double &weight) {
    m_Energy = energy;
    m_E0m = m_Energy / electron_mass_c2;
    m_InvWlPhoton = std::sqrt(0.5) * cm * m_Energy / (h_Planck * c_light); // sqrt(0.5) for trigo reasons, see comment when used

    G4double cs = m_CrossSectionHandler->FindValue(Z, energy);
    m_Z = Z;
    m_eRadiusOverCrossSectionTerm = weight * ( classic_electr_radius*classic_electr_radius) / (2.*cs);
  }

private:
  VectorType           m_Direction;
  double               m_Energy;
  double               m_E0m;
  double               m_InvWlPhoton;
  unsigned int         m_Z;
  double               m_eRadiusOverCrossSectionTerm;

  // Compton data
  G4VEMDataSet* m_ScatterFunctionData;
  G4VCrossSectionHandler* m_CrossSectionHandler;
};
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
class RayleighValueAccumulation:
    public VAccumulation
{
public:

  RayleighValueAccumulation() {
    // G4 data
    G4VDataSetAlgorithm* ffInterpolation = new G4LogLogInterpolation;
    G4String formFactorFile = "rayl/re-ff-";
    m_FormFactorData = new G4CompositeEMDataSet( ffInterpolation, 1., 1.);
    m_FormFactorData->LoadData(formFactorFile);

    m_CrossSectionHandler = new G4CrossSectionHandler;
    G4String crossSectionFile = "rayl/re-cs-";
    m_CrossSectionHandler->LoadData(crossSectionFile);
  }
  ~RayleighValueAccumulation() {
    delete m_FormFactorData;
    delete m_CrossSectionHandler;
  }

  inline double operator()( const rtk::ThreadIdType threadId,
                            double input,
                            const double &itkNotUsed(rayCastValue),
                            const VectorType &stepInMM,
                            const VectorType &source,
                            const VectorType &sourceToPixel,
                            const VectorType &nearestPoint,
                            const VectorType &farthestPoint) const
  {
    // Compute ray length in world material
    // This is used to compute the length in world as well as the direction
    // of the ray in mm.
    VectorType worldVector = sourceToPixel - 2.*source + nearestPoint + farthestPoint;
    for(int i=0; i<3; i++)
      worldVector[i] *= m_VolumeSpacing[i];
    const double worldVectorNorm = worldVector.GetNorm();

    // This is taken from GateDiffCrossSectionActor.cc and simplified
    double cosT = worldVector * m_Direction / worldVectorNorm;
    double DCSThomsonTerm1 = (1 + cosT * cosT);
    double DCSThomson = m_eRadiusOverCrossSectionTerm * DCSThomsonTerm1;
    double x = std::sqrt(1.-cosT) * m_InvWlPhoton;// 1-cosT=2*sin(T/2)^2
    double formFactor = m_FormFactorData->FindValue(x, m_Z-1);
    double DCSrayleigh = DCSThomson * formFactor * formFactor;

    // Multiply interpolation weights by step norm in MM to convert voxel
    // intersection length to MM.
    const double stepInMMNorm = stepInMM.GetNorm();
    for(unsigned int j=0; j<m_InterpolationWeights[threadId].size()-1; j++)
      m_InterpolationWeights[threadId][j] *= stepInMMNorm;

    // The last material is the world material. One must fill the weight with
    // the length from farthest point to pixel point.
    m_InterpolationWeights[threadId].back() = worldVectorNorm;

    // Ray integral
    double rayIntegral = 0.;
    for(unsigned int j=0; j<m_InterpolationWeights[threadId].size(); j++)
      rayIntegral += m_InterpolationWeights[threadId][j] * *(m_MaterialMuPointer+j);

    // Final computation
    input += vcl_exp(-rayIntegral) * DCSrayleigh * GetSolidAngle(sourceToPixel);

    // Reset weights for next ray in thread.
    std::fill(m_InterpolationWeights[threadId].begin(), m_InterpolationWeights[threadId].end(), 0.);
    return input;
  }

  void SetDirection(const VectorType &_arg){ m_Direction = _arg; }
  void SetEnergyAndZ(const double  &energy, const unsigned int &Z, const double &weight) {
    m_InvWlPhoton = std::sqrt(0.5) * cm * energy / (h_Planck * c_light); // sqrt(0.5) for trigo reasons, see comment when used

    unsigned int e = itk::Math::Round<double, double>(energy / m_MaterialMu->GetSpacing()[1]);
    m_MaterialMuPointer = m_MaterialMu->GetPixelContainer()->GetBufferPointer();
    m_MaterialMuPointer += e * m_MaterialMu->GetLargestPossibleRegion().GetSize()[0];

    G4double cs = m_CrossSectionHandler->FindValue(Z, energy);
    m_Z = Z;
    m_eRadiusOverCrossSectionTerm = weight * ( classic_electr_radius*classic_electr_radius) / (2.*cs);
  }

private:
  VectorType           m_Direction;
  double              *m_MaterialMuPointer;
  double               m_InvWlPhoton;
  unsigned int         m_Z;
  double               m_eRadiusOverCrossSectionTerm;

  // G4 data
  G4VEMDataSet* m_FormFactorData;
  G4VCrossSectionHandler* m_CrossSectionHandler;
};
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
namespace Functor
{
  template< class TInput1, class TInput2 = TInput1, class TOutput = TInput1 >
  class Attenuation
  {
    public:
    Attenuation() {}
    ~Attenuation() {}
    bool operator!=(const Attenuation &) const
    {
      return false;
    }

    bool operator==(const Attenuation & other) const
    {
      return !( *this != other );
    }

    inline TOutput operator()(const TInput1 A,const TInput2 B) const
    {
      //Calculating attenuation image (-log(primaryImage/flatFieldImage))
      return (TOutput)(-log(A/B));
    }
  };
}
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
class GateHybridForcedDetectionActorMessenger;
class GateHybridForcedDetectionActor : public GateVActor
{
public:

  //-----------------------------------------------------------------------------
  // This macro initialize the CreatePrototype and CreateInstance
  FCT_FOR_AUTO_CREATOR_ACTOR(GateHybridForcedDetectionActor)

  GateHybridForcedDetectionActor(G4String name, G4int depth=0);
  virtual ~GateHybridForcedDetectionActor();

  // Constructs the actor
  virtual void Construct();

  // Callbacks
  virtual void BeginOfRunAction(const G4Run*);
  virtual void BeginOfEventAction(const G4Event*);
  // virtual void PreUserTrackingAction(const GateVVolume *, const G4Track*);
  virtual void UserSteppingAction(const GateVVolume *, const G4Step*); 

  /// Saves the data collected to the file
  virtual void SaveData();
  virtual void ResetData();

  // Resolution of the detector plane (2D only, z=1);
  const G4ThreeVector & GetDetectorResolution() const { return mDetectorResolution; }
  void SetDetectorResolution(int x, int y) { mDetectorResolution[0] = x; mDetectorResolution[1] = y; }
  void SetDetectorVolumeName(G4String name) { mDetectorName = name; }
  void SetGeometryFilename(G4String name) { mGeometryFilename = name; }
  void SetPrimaryFilename(G4String name) { mPrimaryFilename = name; }
  void SetMaterialMuFilename(G4String name) { mMaterialMuFilename = name; }
  void SetAttenuationFilename(G4String name) { mAttenuationFilename = name; }
  void SetFlatFieldFilename(G4String name) { mFlatFieldFilename = name; }
  void SetComptonFilename(G4String name) { mComptonFilename = name; }
  void SetRayleighFilename(G4String name) { mRayleighFilename = name; }

  // Typedef for rtk
  static const unsigned int Dimension = 3;
  typedef float                                       InputPixelType;
  typedef itk::Image<InputPixelType, Dimension>       InputImageType;
  typedef itk::Image<int, Dimension>                  IntegerImageType;
  typedef itk::Image<double, Dimension>               DoubleImageType;
  typedef float                                       OutputPixelType;
  typedef itk::Image<OutputPixelType, Dimension>      OutputImageType;
  typedef rtk::Reg23ProjectionGeometry                GeometryType;
  typedef rtk::Reg23ProjectionGeometry::PointType     PointType;
  typedef rtk::Reg23ProjectionGeometry::VectorType    VectorType;
  typedef rtk::ConstantImageSource< OutputImageType > ConstantImageSourceType;
  
  void ComputeGeometryInfoInImageCoordinateSystem(GateVImageVolume *image,
                                                  GateVVolume *detector,
                                                  GateVSource *src,
                                                  PointType &primarySourcePosition,
                                                  PointType &detectorPosition,
                                                  VectorType &detectorRowVector,
                                                  VectorType &detectorColVector);
  InputImageType::Pointer ConvertGateImageToITKImage(GateVImageVolume * gateImgVol);
  InputImageType::Pointer CreateVoidProjectionImage();

protected:
  GateHybridForcedDetectionActorMessenger * pActorMessenger;
  
  G4String mDetectorName;
  G4EmCalculator * mEMCalculator;
  GateVVolume * mDetector;
  GateVSource * mSource;
  G4String mGeometryFilename;
  G4String mPrimaryFilename;
  G4String mMaterialMuFilename;
  G4String mAttenuationFilename;
  G4String mFlatFieldFilename;
  G4String mComptonFilename;
  G4String mRayleighFilename;

  G4ThreeVector mDetectorResolution;

  GeometryType::Pointer mGeometry;
  InputImageType::Pointer mGateVolumeImage;
  InputImageType::Pointer mPrimaryImage;
  InputImageType::Pointer mFlatFieldImage;
  InputImageType::Pointer mComptonImage;
  InputImageType::Pointer mRayleighImage;
  std::vector<InputImageType::Pointer> mComptonPerOrderImages;
  std::vector<InputImageType::Pointer> mRayleighPerOrderImages;

  // Geometry information initialized at the beginning of the run
  G4AffineTransform m_WorldToCT;
  PointType mDetectorPosition;
  VectorType mDetectorRowVector;
  VectorType mDetectorColVector;

  // Primary stuff
  itk::TimeProbe mPrimaryProbe;
  unsigned int mNumberOfEventsInRun;
  typedef rtk::JosephForwardProjectionImageFilter< InputImageType,
                                                   InputImageType,
                                                   InterpolationWeightMultiplication,
                                                   PrimaryValueAccumulation> PrimaryProjectionType;

  // Compton stuff
  itk::TimeProbe mComptonProbe;
  typedef rtk::JosephForwardProjectionImageFilter< InputImageType,
                                                   InputImageType,
                                                   InterpolationWeightMultiplication,
                                                   ComptonValueAccumulation> ComptonProjectionType;
  ComptonProjectionType::Pointer mComptonProjector;

  // Rayleigh stuff
  itk::TimeProbe mRayleighProbe;
  typedef rtk::JosephForwardProjectionImageFilter< InputImageType,
                                                   InputImageType,
                                                   InterpolationWeightMultiplication,
                                                   RayleighValueAccumulation> RayleighProjectionType;
  RayleighProjectionType::Pointer mRayleighProjector;
};
//-----------------------------------------------------------------------------

MAKE_AUTO_CREATOR_ACTOR(HybridForcedDetectionActor, GateHybridForcedDetectionActor)


#endif /* end #define GATEHYBRIDFORCEDDECTECTIONACTOR_HH */

#endif // GATE_USE_RTK
