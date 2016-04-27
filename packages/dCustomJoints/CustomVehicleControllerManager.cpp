/* Copyright (c) <2009> <Newton Game Dynamics>
* 
* This software is provided 'as-is', without any express or implied
* warranty. In no event will the authors be held liable for any damages
* arising from the use of this software.
* 
* Permission is granted to anyone to use this software for any purpose,
* including commercial applications, and to alter it and redistribute it
* freely
*/


// NewtonCustomJoint.cpp: implementation of the NewtonCustomJoint class.
//
//////////////////////////////////////////////////////////////////////
#include "CustomJointLibraryStdAfx.h"
#include "CustomJoint.h"
#include "CustomHinge.h"
#include "CustomVehicleControllerManager.h"

//#define D_PLOT_ENGINE_CURVE

#ifdef D_PLOT_ENGINE_CURVE 
static FILE* file_xxx;
#endif

//static int xxx;


#define D_VEHICLE_NEUTRAL_GEAR			0
#define D_VEHICLE_REVERSE_GEAR			1
#define D_VEHICLE_FIRST_GEAR			2
#define D_VEHICLE_REST_GAS_VALVE		dFloat(0.1f)
#define D_VEHICLE_MIN_RPM_FACTOR		dFloat(0.5f)
#define D_VEHICLE_MAX_DRIVETRAIN_DOF	32
#define D_VEHICLE_REGULARIZER			1.0001f
#define D_VEHICLE_SLIP_DIFF_ENGAGE_RPS	4.0f
#define D_VEHICLE_SLIP_DIFF_TORQUE		4000.0f

/*
class CustomVehicleController::dWeightDistibutionSolver: public dSymmetricBiconjugateGradientSolve
{
	public:
	dWeightDistibutionSolver()
		:dSymmetricBiconjugateGradientSolve()
		,m_count(0)
	{
	}

	virtual void MatrixTimeVector(dFloat64* const out, const dFloat64* const v) const
	{
		dComplentaritySolver::dJacobian invMassJacobians;
		invMassJacobians.m_linear = dVector(0.0f);
		invMassJacobians.m_angular = dVector(0.0f);
		for (int i = 0; i < m_count; i++) {
			invMassJacobians.m_linear += m_invMassJacobians[i].m_linear.Scale(dFloat(v[i]));
			invMassJacobians.m_angular += m_invMassJacobians[i].m_angular.Scale(dFloat(v[i]));
		}

		for (int i = 0; i < m_count; i++) {
			out[i] = m_diagRegularizer[i] * v[i] + invMassJacobians.m_linear % m_jacobians[i].m_linear + invMassJacobians.m_angular % m_jacobians[i].m_angular;
		}
	}

	virtual bool InversePrecoditionerTimeVector(dFloat64* const out, const dFloat64* const v) const
	{
		for (int i = 0; i < m_count; i++) {
			out[i] = v[i] * m_invDiag[i];
		}
		return true;
	}

	dComplentaritySolver::dJacobian m_jacobians[256];
	dComplentaritySolver::dJacobian m_invMassJacobians[256];
	dFloat m_invDiag[256];
	dFloat m_diagRegularizer[256];
	int m_count;
};
*/

class CustomVehicleControllerManager::TireFilter: public CustomControllerConvexCastPreFilter
{
	public:
	TireFilter(const CustomVehicleController::BodyPartTire* const tire, const NewtonBody* const vehicle)
		:CustomControllerConvexCastPreFilter(tire->GetBody())
		,m_tire (tire)
		,m_vehicle(vehicle)
	{
	}

	unsigned Prefilter(const NewtonBody* const body, const NewtonCollision* const myCollision)
	{
		dAssert(body != m_me);
		for (int i = 0; i < m_tire->m_collidingCount; i ++) {
			if (m_tire->m_contactInfo[i].m_hitBody == body) {
				return 0;
			}
		}

		return (body != m_vehicle) ? 1 : 0;
	}

	const NewtonBody* m_vehicle;
	const CustomVehicleController::BodyPartTire* m_tire;
};


void CustomVehicleController::dInterpolationCurve::InitalizeCurve(int points, const dFloat* const steps, const dFloat* const values)
{
	m_count = points;
	dAssert(points <= int(sizeof(m_nodes) / sizeof (m_nodes[0])));
	memset(m_nodes, 0, sizeof (m_nodes));
	for (int i = 0; i < m_count; i++) {
		m_nodes[i].m_param = steps[i];
		m_nodes[i].m_value = values[i];
	}
}

dFloat CustomVehicleController::dInterpolationCurve::GetValue(dFloat param) const
{
	dFloat interplatedValue = 0.0f;
	if (m_count) {
		param = dClamp(param, dFloat(0.0f), m_nodes[m_count - 1].m_param);
		interplatedValue = m_nodes[m_count - 1].m_value;
		for (int i = 1; i < m_count; i++) {
			if (param < m_nodes[i].m_param) {
				dFloat df = m_nodes[i].m_value - m_nodes[i - 1].m_value;
				dFloat ds = m_nodes[i].m_param - m_nodes[i - 1].m_param;
				dFloat step = param - m_nodes[i - 1].m_param;

				interplatedValue = m_nodes[i - 1].m_value + df * step / ds;
				break;
			}
		}
	}
	return interplatedValue;
}

class CustomVehicleController::WheelJoint: public CustomJoint
{
	public:
	WheelJoint (const dMatrix& pinAndPivotFrame, NewtonBody* const tire, NewtonBody* const parentBody, BodyPartTire* const tireData)
		:CustomJoint (6, tire, parentBody)
		,m_lateralDir(0.0f, 0.0f, 0.0f, 0.0f)
		,m_longitudinalDir(0.0f, 0.0f, 0.0f, 0.0f)
		,m_tire (tireData)
		,m_tireLoad(0.0f)
		,m_steerRate (0.25f * 3.1416f)
		,m_steerAngle0(0.0f)
		,m_steerAngle1(0.0f)
		,m_brakeTorque(0.0f)
	{
		CalculateLocalMatrix (pinAndPivotFrame, m_localMatrix0, m_localMatrix1);
	}

	dFloat CalculateTireParametricPosition(const dMatrix& tireMatrix, const dMatrix& chassisMatrix) const 
	{
		const dVector& chassisP0 = chassisMatrix.m_posit;
		dVector chassisP1(chassisMatrix.m_posit + chassisMatrix.m_up.Scale(m_tire->m_data.m_suspesionlenght));
		dVector p1p0(chassisP1 - chassisP0);
		dVector q1p0(tireMatrix.m_posit - chassisP0);
		dFloat num = q1p0 % p1p0;
		dFloat den = p1p0 % p1p0;
		return num / den;
	}

	void ApplyBumperImpactLimit(const dVector& dir, dFloat param)
	{
		dComplentaritySolver::dJacobian tireJacobian;
		dComplentaritySolver::dJacobian chassisJacobian;

		dMatrix tireMatrix;
		dMatrix tireInvInertia;
		dMatrix chassisMatrix;
		dMatrix chassisInvInertia;
		dVector com(0.0f);
		dVector tireVeloc(0.0f);
		dVector tireOmega(0.0f);
		dVector chassisVeloc(0.0f);
		dVector chassisOmega(0.0f);

		dFloat tireInvMass;
		dFloat chassisInvMass;
		dFloat Ixx;
		dFloat Iyy;
		dFloat Izz;

		NewtonBody* const tire = m_body0;
		NewtonBody* const chassis = m_body1;
		NewtonBodyGetMatrix(tire, &tireMatrix[0][0]);
		NewtonBodyGetMatrix(chassis, &chassisMatrix[0][0]);
		NewtonBodyGetCentreOfMass(chassis, &com[0]);

		NewtonBodyGetOmega(tire, &tireOmega[0]);
		NewtonBodyGetVelocity(tire, &tireVeloc[0]);
		NewtonBodyGetOmega(chassis, &chassisOmega[0]);
		NewtonBodyGetVelocity(chassis, &chassisVeloc[0]);


		dVector r (tireMatrix.m_posit - chassisMatrix.TransformVector(com));
		tireJacobian.m_linear = dir;
		tireJacobian.m_angular = dVector (0.0f);
		chassisJacobian.m_linear = dir.Scale (-1.0f);
		chassisJacobian.m_angular = r * chassisJacobian.m_linear;

		dFloat relativeVeloc = tireVeloc % tireJacobian.m_linear + tireOmega % tireJacobian.m_angular + chassisVeloc % chassisJacobian.m_linear + chassisOmega % chassisJacobian.m_angular;
		if (relativeVeloc > 0.0f) {
			dComplentaritySolver::dJacobian tireInvMassJacobianTrans;
			dComplentaritySolver::dJacobian chassisInvMassJacobianTrans;

			NewtonBodyGetInvMass(tire, &tireInvMass, &Ixx, &Ixx, &Ixx);
			NewtonBodyGetInvMass(chassis, &chassisInvMass, &Ixx, &Iyy, &Izz);
			NewtonBodyGetInvInertiaMatrix(tire, &tireInvInertia[0][0]);
			NewtonBodyGetInvInertiaMatrix(chassis, &chassisInvInertia[0][0]);

			tireInvMassJacobianTrans.m_linear = tireJacobian.m_linear.Scale (tireInvMass);
			tireInvMassJacobianTrans.m_angular = tireInvInertia.RotateVector(tireJacobian.m_angular);

			chassisInvMassJacobianTrans.m_linear = chassisJacobian.m_linear.Scale(tireInvMass);
			chassisInvMassJacobianTrans.m_angular = chassisInvInertia.RotateVector (chassisJacobian.m_angular);

			dFloat den = tireJacobian.m_linear % tireInvMassJacobianTrans.m_linear + tireJacobian.m_angular % tireInvMassJacobianTrans.m_angular + 
						 chassisJacobian.m_linear % chassisInvMassJacobianTrans.m_linear + chassisJacobian.m_angular % chassisInvMassJacobianTrans.m_angular;

			dFloat impulse = - relativeVeloc / den;
			tireVeloc += tireInvMassJacobianTrans.m_linear.Scale (impulse);
			tireOmega += tireInvMassJacobianTrans.m_angular.Scale (impulse);
			chassisVeloc += chassisInvMassJacobianTrans.m_linear.Scale (impulse);
			chassisOmega += chassisInvMassJacobianTrans.m_angular.Scale (impulse);

			NewtonBodySetOmega(tire, &tireOmega[0]);
			NewtonBodySetVelocity(tire, &tireVeloc[0]);
			NewtonBodySetOmega(chassis, &chassisOmega[0]);
			NewtonBodySetVelocity(chassis, &chassisVeloc[0]);
		}
	}

	void RemoveKinematicError(dFloat timestep)
	{
		dMatrix tireMatrix;
		dMatrix chassisMatrix;
		dVector tireVeloc(0.0f);
		dVector tireOmega(0.0f);
		dVector chassisVeloc(0.0f);
		dVector chassisOmega(0.0f);

		CalculateGlobalMatrix(tireMatrix, chassisMatrix);

		if (m_steerAngle0 < m_steerAngle1) {
			m_steerAngle0 += m_steerRate * timestep;
			if (m_steerAngle0 > m_steerAngle1) {
				m_steerAngle0 = m_steerAngle1;
			}
		} else if (m_steerAngle0 > m_steerAngle1) {
			m_steerAngle0 -= m_steerRate * timestep;
			if (m_steerAngle0 < m_steerAngle1) {
				m_steerAngle0 = m_steerAngle1;
			}
		}

		chassisMatrix = dYawMatrix(m_steerAngle0) * chassisMatrix;

		tireMatrix.m_front = chassisMatrix.m_front;
		tireMatrix.m_right = tireMatrix.m_front * tireMatrix.m_up;
		tireMatrix.m_right = tireMatrix.m_right.Scale(1.0f / dSqrt(tireMatrix.m_right % tireMatrix.m_right));
		tireMatrix.m_up = tireMatrix.m_right * tireMatrix.m_front;

		dFloat param = 0.0f;
		if (!m_tire->GetController()->m_isAirborned) {
			param = CalculateTireParametricPosition (tireMatrix, chassisMatrix);
			if (param > 1.0f) {
				param = 1.0f;
				ApplyBumperImpactLimit(chassisMatrix.m_up, param);
			} else if (param < 0.0f){
				param = 0.0f;
				ApplyBumperImpactLimit(chassisMatrix.m_up, param);
			}
		}
		
		tireMatrix.m_posit = chassisMatrix.m_posit + chassisMatrix.m_up.Scale (param * m_tire->m_data.m_suspesionlenght);

		NewtonBody* const tire = m_body0;
		NewtonBody* const chassis = m_body1;

		tireMatrix = GetMatrix0().Inverse() * tireMatrix;
		NewtonBodyGetVelocity(tire, &tireVeloc[0]);
		NewtonBodyGetPointVelocity(chassis, &tireMatrix.m_posit[0], &chassisVeloc[0]);
		chassisVeloc -= chassisMatrix.m_up.Scale (chassisVeloc % chassisMatrix.m_up);
		tireVeloc = chassisVeloc + chassisMatrix.m_up.Scale (tireVeloc % chassisMatrix.m_up);
		
		NewtonBodyGetOmega(tire, &tireOmega[0]);
		NewtonBodyGetOmega(chassis, &chassisOmega[0]);
		tireOmega = chassisOmega + tireMatrix.m_front.Scale (tireOmega % tireMatrix.m_front);

		NewtonBodySetMatrixNoSleep(tire, &tireMatrix[0][0]);
		NewtonBodySetVelocityNoSleep(tire, &tireVeloc[0]);
		NewtonBodySetOmegaNoSleep(tire, &tireOmega[0]);
	}

	void SubmitConstraints(dFloat timestep, int threadIndex)
	{
		dMatrix tireMatrix;
		dMatrix chassisMatrix;
		dVector tireOmega(0.0f);
		dVector chassisOmega(0.0f);
		dVector tireVeloc(0.0f);
		dVector chassisCom(0.0f);
		dVector chassisVeloc(0.0f);

		NewtonBody* const tire = m_body0;
		NewtonBody* const chassis = m_body1;
		dAssert (m_body0 == m_tire->GetBody());
		dAssert (m_body1 == m_tire->GetParent()->GetBody());

		// calculate the position of the pivot point and the Jacobian direction vectors, in global space. 
		CalculateGlobalMatrix(tireMatrix, chassisMatrix);
		chassisMatrix = dYawMatrix(m_steerAngle0) * chassisMatrix;

		m_lateralDir = chassisMatrix.m_front;
		m_longitudinalDir = chassisMatrix.m_right;

		NewtonUserJointAddLinearRow(m_joint, &tireMatrix.m_posit[0], &chassisMatrix.m_posit[0], &m_lateralDir[0]);
		NewtonUserJointSetRowAcceleration(m_joint, NewtonUserCalculateRowZeroAccelaration(m_joint));

		NewtonUserJointAddLinearRow(m_joint, &tireMatrix.m_posit[0], &chassisMatrix.m_posit[0], &m_longitudinalDir[0]);
		NewtonUserJointSetRowAcceleration(m_joint, NewtonUserCalculateRowZeroAccelaration(m_joint));
		
		NewtonBodyGetOmega(tire, &tireOmega[0]);
		NewtonBodyGetOmega(chassis, &chassisOmega[0]);
		dVector relOmega(tireOmega - chassisOmega);

		dFloat angle = -CalculateAngle(tireMatrix.m_front, chassisMatrix.m_front, chassisMatrix.m_right);
		dFloat omega = relOmega % chassisMatrix.m_right;
		dFloat alphaError = -(angle + omega * timestep) / (timestep * timestep);
		NewtonUserJointAddAngularRow(m_joint, -angle, &chassisMatrix.m_right[0]);
		NewtonUserJointSetRowAcceleration(m_joint, alphaError);

		angle = CalculateAngle(tireMatrix.m_front, chassisMatrix.m_front, chassisMatrix.m_up);
		omega = relOmega % chassisMatrix.m_up;
		alphaError = -(angle + omega * timestep) / (timestep * timestep);
		NewtonUserJointAddAngularRow(m_joint, -angle, &chassisMatrix.m_up[0]);
		NewtonUserJointSetRowAcceleration(m_joint, alphaError);

		dFloat param = CalculateTireParametricPosition(tireMatrix, chassisMatrix);
		if (param >= 1.0f) {
			dVector chassisMatrixPosit (chassisMatrix.m_posit + chassisMatrix.m_up.Scale (param * m_tire->m_data.m_suspesionlenght));
			NewtonUserJointAddLinearRow(m_joint, &tireMatrix.m_posit[0], &chassisMatrix.m_posit[0], &chassisMatrix.m_up[0]);
			NewtonUserJointSetRowSpringDamperAcceleration(m_joint, m_tire->m_data.m_springStrength, m_tire->m_data.m_dampingRatio);
			NewtonUserJointSetRowMaximumFriction(m_joint, 0.0f);
		} else if (param <= 0.0f) {
			NewtonUserJointAddLinearRow(m_joint, &tireMatrix.m_posit[0], &chassisMatrix.m_posit[0], &chassisMatrix.m_up[0]);
			if (!m_tire->GetController()->m_isAirborned) {
				NewtonUserJointSetRowMinimumFriction(m_joint, 0.0f);
			}
		} else {
			NewtonUserJointAddLinearRow(m_joint, &tireMatrix.m_posit[0], &chassisMatrix.m_posit[0], &chassisMatrix.m_up[0]);
			NewtonUserJointSetRowSpringDamperAcceleration(m_joint, m_tire->m_data.m_springStrength, m_tire->m_data.m_dampingRatio);
		}

		if (m_brakeTorque > 1.0e-3f) {
			dFloat speed = relOmega % m_lateralDir;
			NewtonUserJointAddAngularRow(m_joint, 0.0f, &m_lateralDir[0]);
			NewtonUserJointSetRowAcceleration(m_joint, -speed / timestep);
			NewtonUserJointSetRowMinimumFriction(m_joint, -m_brakeTorque);
			NewtonUserJointSetRowMaximumFriction(m_joint, m_brakeTorque);
		}
		m_brakeTorque = 0.0f;
	}

	dFloat GetTireLoad () const
	{
		return m_tireLoad;
	}

	dVector GetLongitudinalForce() const
	{
		return m_longitudinalDir.Scale(NewtonUserJointGetRowForce(m_joint, 1));
	}

	dVector GetLateralForce() const
	{
		return m_lateralDir.Scale (NewtonUserJointGetRowForce (m_joint, 0));
	}

	dVector m_lateralDir;
	dVector m_longitudinalDir;
	BodyPartTire* m_tire;
	dFloat m_tireLoad;
	dFloat m_steerRate;
	dFloat m_steerAngle0;
	dFloat m_steerAngle1;
	dFloat m_brakeTorque;
};

void CustomVehicleController::BodyPartChassis::ApplyDownForce ()
{
	// add aerodynamics forces
	dMatrix matrix;
	dVector veloc(0.0f);

	NewtonBody* const body = GetBody();
	NewtonBodyGetVelocity(body, &veloc[0]);
	NewtonBodyGetMatrix(body, &matrix[0][0]);
	matrix = GetController()->m_localFrame * matrix;

	veloc -= matrix.m_up.Scale (veloc % matrix.m_up);
	dFloat downForceMag = m_aerodynamicsDownForceCoefficient * (veloc % veloc);
	if (downForceMag > m_aerodynamicsDownForce0) {
		dFloat speed = dSqrt (veloc % veloc);
		dFloat topSpeed = GetController()->GetEngine() ? GetController()->GetEngine()->GetTopGear() : 30.0f;
		dFloat speedRatio = (speed - m_aerodynamicsDownSpeedCutOff) / (topSpeed - speed); 
		downForceMag = m_aerodynamicsDownForce0 + (m_aerodynamicsDownForce1 - m_aerodynamicsDownForce0) * speedRatio; 
	}

	dVector downforce(matrix.m_up.Scale (-downForceMag));
	NewtonBodyAddForce(body, &downforce[0]);
}

CustomVehicleController::BodyPartTire::BodyPartTire()
	:BodyPart()
	,m_lateralSlip(0.0f)
	,m_longitudinalSlip(0.0f)
	,m_aligningTorque(0.0f)
	,m_driveTorque(0.0f)
	,m_index(0)
	,m_collidingCount(0)
{
}

CustomVehicleController::BodyPartTire::~BodyPartTire()
{
}

void CustomVehicleController::BodyPartTire::Init (BodyPart* const parentPart, const dMatrix& locationInGlobalSpase, const Info& info)
{
	m_data = info;
	m_parent = parentPart;
	m_userData = info.m_userData;
	m_controller = parentPart->m_controller;

	m_collidingCount = 0;
	m_lateralSlip = 0.0f;
	m_aligningTorque = 0.0f;
	m_longitudinalSlip = 0.0f;

	CustomVehicleControllerManager* const manager = (CustomVehicleControllerManager*)m_controller->GetManager();

	NewtonWorld* const world = ((CustomVehicleControllerManager*)m_controller->GetManager())->GetWorld();
	NewtonCollisionSetScale(manager->m_tireShapeTemplate, m_data.m_width, m_data.m_radio, m_data.m_radio);

	// create the rigid body that will make this bone
	dMatrix matrix (dYawMatrix(-0.5f * 3.1415927f) * locationInGlobalSpase);
	m_body = NewtonCreateDynamicBody(world, manager->m_tireShapeTemplate, &matrix[0][0]);
	NewtonCollision* const collision = NewtonBodyGetCollision(m_body);
	NewtonCollisionSetUserData1 (collision, this);
	
	NewtonBodySetMaterialGroupID(m_body, manager->GetTireMaterial());

	dVector drag(0.0f, 0.0f, 0.0f, 0.0f);
	NewtonBodySetLinearDamping(m_body, 0);
	NewtonBodySetAngularDamping(m_body, &drag[0]);
	NewtonBodySetMaxRotationPerStep(m_body, 3.141692f);
	
	// set the standard force and torque call back
	NewtonBodySetForceAndTorqueCallback(m_body, m_controller->m_forceAndTorque);

	// tire are highly non linear, sung spherical inertia matrix make the calculation more accurate 
	dFloat inertia = 2.0f * m_data.m_mass * m_data.m_radio * m_data.m_radio / 5.0f;
	NewtonBodySetMassMatrix (m_body, m_data.m_mass, inertia, inertia, inertia);

	m_joint = new WheelJoint (matrix, m_body, parentPart->m_body, this);
}

dFloat CustomVehicleController::BodyPartTire::GetRPM() const
{
	dVector omega(0.0f); 
	WheelJoint* const joint = (WheelJoint*) m_joint;
	NewtonBodyGetOmega(m_body, &omega[0]);
	return (joint->m_lateralDir % omega) * 9.55f;
}


void CustomVehicleController::BodyPartTire::SetSteerAngle (dFloat angle)
{
	WheelJoint* const tire = (WheelJoint*)m_joint;
	tire->m_steerAngle1 = angle;
}

void CustomVehicleController::BodyPartTire::SetBrakeTorque(dFloat torque)
{
	WheelJoint* const tire = (WheelJoint*)m_joint;
	tire->m_brakeTorque = dMax (torque, tire->m_brakeTorque);
}


void CustomVehicleController::EngineController::Info::ConvertToMetricSystem()
{
	const dFloat horsePowerToWatts = 735.5f;
	const dFloat kmhToMetersPerSecunds = 0.278f;
	const dFloat rpmToRadiansPerSecunds = 0.105f;
	const dFloat poundFootToNewtonMeters = 1.356f;

	m_idleTorque *= poundFootToNewtonMeters;
	m_peakTorque *= poundFootToNewtonMeters;
	m_redLineTorque *= poundFootToNewtonMeters;

	m_rpmAtPeakTorque *= rpmToRadiansPerSecunds;
	m_rpmAtPeakHorsePower *= rpmToRadiansPerSecunds;
	m_rpmAtReadLineTorque *= rpmToRadiansPerSecunds;
	m_rpmAtIdleTorque *= rpmToRadiansPerSecunds;

	m_peakHorsePower *= horsePowerToWatts;
	m_vehicleTopSpeed *= kmhToMetersPerSecunds;

	m_peakPowerTorque = m_peakHorsePower / m_rpmAtPeakHorsePower;

	dAssert(m_rpmAtIdleTorque > 0.0f);
	dAssert(m_rpmAtIdleTorque < m_rpmAtPeakHorsePower);
	dAssert(m_rpmAtPeakTorque < m_rpmAtPeakHorsePower);
	dAssert(m_rpmAtPeakHorsePower < m_rpmAtReadLineTorque);

	dAssert(m_idleTorque > 0.0f);
	dAssert(m_peakTorque > m_peakPowerTorque);
	dAssert(m_peakPowerTorque > m_redLineTorque);
	dAssert(m_redLineTorque > 0.0f);
	dAssert((m_peakTorque * m_rpmAtPeakTorque) < m_peakHorsePower);
}


CustomVehicleController::EngineController::DriveTrain::DriveTrain(const dVector& invInertia, DriveTrain* const parent)
	:CustomAlloc()
	,m_J01(0.0f)
	,m_J10(0.0f)
	,m_invMassJt01(0.0f)
	,m_invMassJt10(0.0f)
	,m_omega(0.0f)
	,m_torque(0.0f)
	,m_inertiaInv(invInertia)
	,m_parent(parent)
	,m_child(NULL)
	,m_sibling(NULL)
	,m_index(0)
{
}

CustomVehicleController::EngineController::DriveTrain::~DriveTrain()
{
	DriveTrain* next;
	for (DriveTrain* ptr = m_child; ptr; ptr = next) {
		next = ptr->m_sibling;
		delete (ptr);
	}
}

void CustomVehicleController::EngineController::DriveTrain::SetInvMassJt()
{
	m_invMassJt01 = m_J01.CompProduct(m_inertiaInv);
	m_invMassJt10 = m_J10.CompProduct(m_parent->m_inertiaInv);
}

void CustomVehicleController::EngineController::DriveTrain::SetPartMasses (const dVector& invInertia)
{
	m_inertiaInv = invInertia;
	for (DriveTrain* ptr = m_child; ptr; ptr = ptr->m_sibling) {
		ptr->SetPartMasses (invInertia);
	}
}

void CustomVehicleController::EngineController::DriveTrain::ReconstructInvMassJt ()
{
	if (m_parent) {
		SetInvMassJt();
	}
	for (DriveTrain* ptr = m_child; ptr; ptr = ptr->m_sibling) {
		ptr->ReconstructInvMassJt();
	}
}

void CustomVehicleController::EngineController::DriveTrain::GetRow(dVector* const row) const
{
	row[m_index] = m_J01;
	row[m_parent->m_index] = m_J10;
}

void CustomVehicleController::EngineController::DriveTrain::GetInvRowT(dVector* const row) const
{
	row[m_index] = m_invMassJt01;
	row[m_parent->m_index] = m_invMassJt10;
}

int CustomVehicleController::EngineController::DriveTrain::GetNodeArray(DriveTrain** const array, int& index)
{
	for (DriveTrain* ptr = m_child; ptr; ptr = ptr->m_sibling) {
		ptr->GetNodeArray(array, index);
		index++;
	}
	array[index] = this;
	return index;
}

int CustomVehicleController::EngineController::DriveTrain::GetNodeArray(DriveTrain** const array)
{
	int index = 0;
	return GetNodeArray(array, index) + 1;
}

void CustomVehicleController::EngineController::DriveTrain::CalculateRightSide(EngineController* const controller, dFloat timestep, dFloat* const rightSide)
{
	const dFloat k = 0.5f / timestep;
	dFloat relativeOmega = m_omega % m_J01 + m_parent->m_omega % m_J10;
	dFloat torqueAccel = m_torque % m_invMassJt01 + m_parent->m_torque % m_invMassJt10;

	torqueAccel = (dAbs(torqueAccel) < 1.0e-8f) ? 0.0f : torqueAccel;
	relativeOmega = (dAbs(relativeOmega) < 1.0e-8f) ? 0.0f : relativeOmega;
	rightSide[m_index] = -(torqueAccel + k * relativeOmega);
}

void CustomVehicleController::EngineController::DriveTrain::FactorRow (int row, int dofSize, dFloat* const massMatrix)
{
	dFloat* const rowI = &massMatrix[row * dofSize];
	for (int j = 0; j <= row; j++) {
		dFloat s = 0.0f;
		const dFloat* const rowJ = &massMatrix[j * dofSize];
		for (int k = 0; k < j; k++) {
			s += rowI[k] * rowJ[k];
		}

		if (j == row) {
			dFloat diag = rowI[row] - s;
			dAssert(diag > 0.0f);
			rowI[row] = dSqrt(diag);
		} else {
			rowI[j] = (1.0f / rowJ[j]) * (rowI[j] - s);
		}
	}
}

void CustomVehicleController::EngineController::DriveTrain::BuildMassMatrix()
{
	const int size = D_VEHICLE_MAX_DRIVETRAIN_DOF;
	DriveTrain* nodeList[size];
	dVector rowI[size];
	dVector rowJ[size];

	int nodeCount = GetNodeArray(nodeList);
	for (int i = 0; i < nodeCount; i++) {
		nodeList[i]->m_index = i;
	}
	int dofSize = nodeCount - 1;
	dAssert(size > dofSize);

	int y = 0;
	dFloat* const massMatrix = GetMassMatrix();
	dAssert (massMatrix);
	memset(massMatrix, 0, dofSize * dofSize * sizeof (dFloat));
	for (int i = 0; i < nodeCount - 1; i++) {
		DriveTrain* const nodeA = nodeList[i];
		int x = 0;
		memset(rowI, 0, nodeCount * sizeof (dVector));
		nodeA->GetRow(rowI);
		dFloat* const row = &massMatrix[y * dofSize];
		for (int j = 0; j < nodeCount - 1; j++) {
			DriveTrain* const nodeB = nodeList[j];
			memset(rowJ, 0, nodeCount * sizeof (dVector));
			nodeB->GetInvRowT(rowJ);

			dFloat acc = 0.0f;
			for (int k = 0; k < nodeCount; k++) {
				acc += rowI[k] % rowJ[k];
			}
			row[x] = acc;
			x++;
		}
		y++;
	}

	for (int i = 0; i < dofSize; i++) {
		massMatrix[i * dofSize + i] *= D_VEHICLE_REGULARIZER;
	}

	for (int i = 0; i < dofSize; i++) {
		FactorRow (i, dofSize, massMatrix);
	}
}

void CustomVehicleController::EngineController::DriveTrain::SetExternalTorque(EngineController* const controller)
{
	m_torque = dVector (0.0f);
}

void CustomVehicleController::EngineController::DriveTrain::Integrate(EngineController* const controller, dFloat timestep)
{
	m_omega += m_inertiaInv.CompProduct(m_torque.Scale(timestep));
}


void CustomVehicleController::EngineController::DriveTrain::ApplyInternalTorque(EngineController* const controller, dFloat timestep, dFloat* const lambda)
{
	m_torque += m_J01.Scale(lambda[m_index]);
	m_parent->m_torque += m_J10.Scale(lambda[m_index]);
}

CustomVehicleController::EngineController::DriveTrainEngine::DriveTrainEngine(const dVector& invInertia)
	:DriveTrain(invInertia)
	,m_lastGear(-1000.0f)
	,m_engineTorque(0.0f)
{
}

void CustomVehicleController::EngineController::DriveTrainEngine::SetExternalTorque(EngineController* const controller)
{
	m_torque = dVector (m_engineTorque, 0.0f, 0.0f, 0.0f);
}

dFloat CustomVehicleController::EngineController::DriveTrainEngine::GetClutchTorque(EngineController* const controller) const
{
	return controller->m_info.m_clutchFrictionTorque * controller->m_clutchParam;
}

void CustomVehicleController::EngineController::DriveTrainEngine::RebuildEngine(const dVector& invInertia)
{
	dVector gearInvInertia(invInertia.Scale(4.0f));
	SetPartMasses(gearInvInertia);
	m_inertiaInv = invInertia;
	ReconstructInvMassJt ();
	BuildMassMatrix();
}

void CustomVehicleController::EngineController::DriveTrainEngine::Solve(int dofSize, int width, const dFloat* const massMatrix, const dFloat* const b, dFloat* const x) const
{
	for (int i = 0; i < dofSize; i++) {
		dFloat acc = 0.0f;
		const dFloat* const rowI = &massMatrix[i * width];
		for (int j = 0; j < i; j++) {
			acc = acc + rowI[j] * x[j];
		}
		x[i] = (b[i] - acc) / rowI[i];
	}

	for (int i = dofSize - 1; i >= 0; i--) {
		dFloat acc = 0.0f;
		for (int j = i + 1; j < dofSize; j++) {
			acc = acc + massMatrix[j * width + i] * x[j];
		}
		x[i] = (x[i] - acc) / massMatrix[i * width + i];
	}
}


void CustomVehicleController::EngineController::DriveTrainEngine::SetGearRatio (dFloat gearRatio)
{
	if (m_lastGear != gearRatio) {
		m_lastGear = gearRatio;
		m_child->SetGearRatioJacobians(gearRatio);

		const int size = D_VEHICLE_MAX_DRIVETRAIN_DOF;
		DriveTrain* nodeList[size];
		dVector rowI[size];
		dVector rowJ[size];

		int nodeCount = GetNodeArray(nodeList);
		int dofSize = nodeCount - 1;
		dAssert(size > dofSize);

		dFloat* const massMatrix = GetMassMatrix();
		dAssert(massMatrix);

		int x = 0;
		DriveTrain* const nodeA = nodeList[nodeCount - 2];
		memset(rowI, 0, nodeCount * sizeof (dVector));
		nodeA->GetRow(rowI);
		int y = dofSize - 1;
		dFloat* const row = &massMatrix[y * dofSize];
		for (int j = 0; j < nodeCount - 1; j++) {
			DriveTrain* const nodeB = nodeList[j];
			memset(rowJ, 0, nodeCount * sizeof (dVector));
			nodeB->GetInvRowT(rowJ);

			dFloat acc = 0.0f;
			for (int k = 0; k < nodeCount; k++) {
				acc += rowI[k] % rowJ[k];
			}
			row[x] = acc;
			massMatrix[y] = acc;
			x++;
			y += dofSize;
		}
		massMatrix[(dofSize - 1) * dofSize + dofSize - 1] *= D_VEHICLE_REGULARIZER;
		FactorRow(dofSize - 1, dofSize, massMatrix);
	}
}

void CustomVehicleController::EngineController::DriveTrainEngine::Update(EngineController* const controller, dFloat engineTorque, dFloat timestep)
{
	const int size = D_VEHICLE_MAX_DRIVETRAIN_DOF;
	DriveTrain* nodeArray[size + 1];
	dFloat b[size];
	dFloat x[size];

	dFloat gearRatio = controller->GetGearRatio();
	SetGearRatio (gearRatio);

	m_engineTorque = engineTorque;
	const int nodesCount = GetNodeArray(nodeArray);
	for (int i = 0; i < nodesCount; i++) {
		DriveTrain* const node = nodeArray[i];
		node->SetExternalTorque(controller);
	}

	for (int i = 0; i < nodesCount - 1; i++) {
		DriveTrain* const node = nodeArray[i];
		node->CalculateRightSide(controller, timestep, b);
	}
	int dofSize = nodesCount - 1;
	dAssert(size >= dofSize);
	const dFloat* const massMatrix = GetMassMatrix();

	int clutchIndex = dofSize - 1;
	if (gearRatio != 0.0f) {
		Solve(dofSize, dofSize, massMatrix, b, x);
		dFloat clutchTorque = GetClutchTorque(controller);
		if (dAbs(x[clutchIndex]) > clutchTorque) {
			dFloat frictionTorque = dClamp (x[clutchIndex], -clutchTorque, clutchTorque);
			x[clutchIndex] = frictionTorque;
			for (int i = 0; i < clutchIndex; i++) {
				b[i] -= massMatrix[i * dofSize + clutchIndex] * frictionTorque;
			}
			Solve(clutchIndex, dofSize, massMatrix, b, x);
		}
	} else {
		x[clutchIndex] = 0.0f;
		Solve(clutchIndex, dofSize, massMatrix, b, x);
	}

	for (int i = 0; i < nodesCount - 1; i++) {
		DriveTrain* const node = nodeArray[i];
		node->ApplyInternalTorque(controller, timestep, x);
	}

	for (int i = 0; i < nodesCount; i++) {
		DriveTrain* const node = nodeArray[i];
		node->Integrate(controller, timestep);
	}
}

CustomVehicleController::EngineController::DriveTrainEngine2W::DriveTrainEngine2W(const dVector& invInertia, const DifferentialAxel& axel)
	:DriveTrainEngine(invInertia)
{
	dVector gearInvInertia(invInertia.Scale(4.0f));
	m_child = new DriveTrainDifferentialGear(gearInvInertia, this, axel);
}

CustomVehicleController::EngineController::DriveTrainEngine4W::DriveTrainEngine4W(const dVector& invInertia, const DifferentialAxel& axel0, const DifferentialAxel& axel1)
	:DriveTrainEngine(invInertia)
{
	dVector gearInvInertia(invInertia.Scale(4.0f));
	m_child = new DriveTrainDifferentialGear(gearInvInertia, this, axel0, axel1);
}

CustomVehicleController::EngineController::DriveTrainDifferentialGear::DriveTrainDifferentialGear(const dVector& invInertia, DriveTrain* const parent, const DifferentialAxel& axel)
	:DriveTrain(invInertia, parent)
	,m_slipDifferentialViscuosFriction(0.1f)
{
	m_child = new DriveTrainTire(axel.m_leftTire, this);
	m_child->m_sibling = new DriveTrainTire(axel.m_rightTire, this);
	SetGearRatioJacobians(1.0f);
}

CustomVehicleController::EngineController::DriveTrainDifferentialGear::DriveTrainDifferentialGear(const dVector& invInertia, DriveTrain* const parent, const DifferentialAxel& axel, dFloat side)
	:DriveTrain(invInertia, parent)
	,m_slipDifferentialViscuosFriction(0.1f)
{
	m_child = new DriveTrainTire(axel.m_leftTire, this);
	m_child->m_sibling = new DriveTrainTire(axel.m_rightTire, this);

	m_J01 = dVector(-1.0f, 0.0f, 0.0f, 0.0f);
	m_J10 = dVector(1.0f, 2.0f * dSign(side), 0.0f, 0.0f);
	SetInvMassJt();
}

CustomVehicleController::EngineController::DriveTrainDifferentialGear::DriveTrainDifferentialGear(const dVector& invInertia, DriveTrain* const parent, const DifferentialAxel& axel0, const DifferentialAxel& axel1)
	:DriveTrain(invInertia, parent)
	,m_slipDifferentialViscuosFriction(0.1f)
{
	m_child = new DriveTrainDifferentialGear(invInertia, this, axel0, -1.0f);
	m_child->m_sibling = new DriveTrainDifferentialGear(invInertia, this, axel1, 1.0f);
	SetGearRatioJacobians(1.0f);
}

void CustomVehicleController::EngineController::DriveTrainDifferentialGear::SetGearRatioJacobians(dFloat gearRatio)
{
	m_J01 = dVector(dAbs (gearRatio), 0.0f, 0.0f, 0.0f);
	m_J10 = dVector(-dSign(gearRatio), 0.0f, 0.0f, 0.0f);
	SetInvMassJt();
}


void CustomVehicleController::EngineController::DriveTrainDifferentialGear::SetExternalTorque(EngineController* const controller)
{
	DriveTrain::SetExternalTorque(controller);
	if (controller->GetSlipDifferential()) {
		dVector omega (m_omega);
		m_omega.m_y = dClamp(m_omega.m_y, -15.0f, 15.0f);
		if (m_omega.m_y > D_VEHICLE_SLIP_DIFF_ENGAGE_RPS) {
			dFloat omegay = m_omega.m_y - D_VEHICLE_SLIP_DIFF_ENGAGE_RPS;
			m_torque.m_y = -omegay * omegay * D_VEHICLE_SLIP_DIFF_TORQUE / (D_VEHICLE_SLIP_DIFF_ENGAGE_RPS * D_VEHICLE_SLIP_DIFF_ENGAGE_RPS);
		} else if (m_omega.m_y < -D_VEHICLE_SLIP_DIFF_ENGAGE_RPS) {
			dFloat omegay = m_omega.m_y + D_VEHICLE_SLIP_DIFF_ENGAGE_RPS;
			m_torque.m_y = omegay * omegay * D_VEHICLE_SLIP_DIFF_TORQUE / (D_VEHICLE_SLIP_DIFF_ENGAGE_RPS * D_VEHICLE_SLIP_DIFF_ENGAGE_RPS);
		}
		dAssert (dAbs(m_torque.m_y) < 50000.0f);
	}
}


CustomVehicleController::EngineController::DriveTrainTire::DriveTrainTire(BodyPartTire* const tire, DriveTrain* const parent)
	:DriveTrain(dVector(0.0f), parent)
	,m_tire(tire)
{
	dFloat massInv;
	NewtonBody* const body = m_tire->GetBody();
	NewtonBodyGetInvMass(body, &massInv, &m_inertiaInv.m_x, &m_inertiaInv.m_y, &m_inertiaInv.m_z);
	dAssert(m_tire->GetJoint()->GetBody0() == body);

	m_J01 = dVector(1.0f, 0.0f, 0.0f, 0.0f);
	m_J10 = dVector (1.0f, 2.0f * dSign(m_tire->m_data.m_location.m_z), 0.0f, 0.0f);
	SetInvMassJt();
}

void CustomVehicleController::EngineController::DriveTrainTire::SetPartMasses (const dVector& invInertia)
{
	dVector tireInvInertia;
	dFloat tireInvMass;

	NewtonBody* const body = m_tire->GetBody();
	NewtonBodyGetInvMass(body, &tireInvMass, &tireInvInertia.m_x, &tireInvInertia.m_y, &tireInvInertia.m_z);
	DriveTrain::SetPartMasses (tireInvInertia);
}

void CustomVehicleController::EngineController::DriveTrainTire::SetExternalTorque(EngineController* const controller)
{
	dMatrix matrix;
	dVector omega(0.0f);

	NewtonBody* const body = m_tire->GetBody();
	NewtonBodyGetOmega(body, &omega[0]);
	NewtonBodyGetMatrix(body, &matrix[0][0]);

	m_omega = matrix.UnrotateVector(omega);
	m_torque = dVector (m_tire->m_driveTorque, 0.0f, 0.0f, 0.0f);
}

void CustomVehicleController::EngineController::DriveTrainTire::ApplyInternalTorque(EngineController* const controller, dFloat timestep, dFloat* const lambda)
{
	dMatrix matrix;
	dVector torque(m_J01.Scale(lambda[m_index]));
	NewtonBody* const tireBody = m_tire->GetBody();
	NewtonBody* const chassisBody = m_tire->GetParent()->GetBody();
	NewtonBodyGetMatrix(tireBody, &matrix[0][0]);
	torque = matrix.RotateVector(torque);
	NewtonBodyAddTorque(tireBody, &torque[0]);

	torque = torque.Scale (-0.25f);
	NewtonBodyAddTorque(chassisBody, &torque[0]);
	
	dVector parentTorque(m_J10.Scale(lambda[m_index]));
	m_parent->m_torque += parentTorque;
}

CustomVehicleController::EngineController::EngineController (CustomVehicleController* const controller, const Info& info, const DifferentialAxel& axel0, const DifferentialAxel& axel1)
	:Controller(controller)
	,m_info(info)
	,m_infoCopy(info)
	,m_engine(NULL)
	,m_clutchParam(1.0f)
	,m_gearTimer(0)
	,m_currentGear(D_VEHICLE_NEUTRAL_GEAR)
	,m_ignitionKey(false)
	,m_automaticTransmissionMode(true)
{
	dFloat inertiaInv = 1.0f / (2.0f * m_info.m_mass * m_info.m_radio * m_info.m_radio / 5.0f);
	if (axel1.m_leftTire) {
		dAssert (axel0.m_leftTire);
		dAssert (axel0.m_rightTire);
		dAssert (axel1.m_leftTire);
		dAssert (axel1.m_rightTire);
		m_engine = new DriveTrainEngine4W (dVector(inertiaInv, inertiaInv, inertiaInv, 0.0f), axel0, axel1);
	} else {
		dAssert (axel0.m_leftTire);
		dAssert (axel0.m_rightTire);
		m_engine = new DriveTrainEngine2W (dVector(inertiaInv, inertiaInv, inertiaInv, 0.0f), axel0);
	}
	SetInfo(info);

	m_controller->SetAerodynamicsDownforceCoefficient(10.0f, 0.5f, 0.4f, 1.0f);
}

CustomVehicleController::EngineController::~EngineController()
{
	if (m_engine) {
		delete m_engine;
	}
}

CustomVehicleController::EngineController::Info CustomVehicleController::EngineController::GetInfo() const
{
	return m_infoCopy;
}

void CustomVehicleController::EngineController::SetInfo(const Info& info)
{
	m_info = info;
	m_infoCopy = info;

	m_info.m_clutchFrictionTorque = dAbs (m_info.m_clutchFrictionTorque);
	m_infoCopy.m_clutchFrictionTorque = m_info.m_clutchFrictionTorque;

	dFloat inertiaInv = 1.0f / (2.0f * m_info.m_mass * m_info.m_radio * m_info.m_radio / 5.0f);
	m_engine->RebuildEngine (dVector (inertiaInv, inertiaInv, inertiaInv, 0.0f));

	InitEngineTorqueCurve();

	dAssert(info.m_gearsCount < (int(sizeof (m_info.m_gearRatios) / sizeof (m_info.m_gearRatios[0])) - D_VEHICLE_FIRST_GEAR));
	m_info.m_gearsCount = info.m_gearsCount + D_VEHICLE_FIRST_GEAR;

	m_info.m_gearRatios[D_VEHICLE_NEUTRAL_GEAR] = 0.0f;
	m_info.m_gearRatios[D_VEHICLE_REVERSE_GEAR] = -dAbs(info.m_reverseGearRatio);
	for (int i = 0; i < (m_info.m_gearsCount - D_VEHICLE_FIRST_GEAR); i++) {
		m_info.m_gearRatios[i + D_VEHICLE_FIRST_GEAR] = dAbs(info.m_gearRatios[i]);
	}

	for (dList<BodyPartTire>::dListNode* tireNode = m_controller->m_tireList.GetFirst(); tireNode; tireNode = tireNode->GetNext()) {
		BodyPartTire& tire = tireNode->GetInfo();
		dFloat angle = (1.0f / 30.0f) * (0.277778f) * info.m_vehicleTopSpeed / tire.m_data.m_radio;
		NewtonBodySetMaxRotationPerStep(tire.GetBody(), angle);
	}
}


bool CustomVehicleController::EngineController::GetSlipDifferential() const
{
	return m_info.m_slipDifferentialOn ? true : false;
}

void CustomVehicleController::EngineController::SetSlipDifferential(bool mode)
{
	m_info.m_slipDifferentialOn ? 1 : 0;
}


dFloat CustomVehicleController::EngineController::GetTopGear() const
{
	return m_info.m_gearRatios[m_info.m_gearsCount - 1];
}


void CustomVehicleController::EngineController::CalculateCrownGear()
{
	dAssert(m_info.m_vehicleTopSpeed >= 0.0f);
	dAssert(m_info.m_vehicleTopSpeed < 100.0f);

	DriveTrain* nodeArray[256];
	int nodesCount = m_engine->GetNodeArray(nodeArray);

	BodyPartTire* tire = NULL;
	for (int i = 0; i < nodesCount; i ++) {
		DriveTrainTire* const tireNode = nodeArray[i]->CastAsTire();
		if (tireNode) {
			tire = tireNode->m_tire;
			break;
		}
	}
	dAssert (tire);

	// drive train geometrical relations
	// G0 = m_differentialGearRatio
	// G1 = m_transmissionGearRatio
	// s = topSpeedMPS
	// r = tireRadio
	// wt = rpsAtTire
	// we = rpsAtPickPower
	// we = G1 * G0 * wt;
	// wt = e / r
	// we = G0 * G1 * s / r
	// G0 = r * we / (G1 * s)
	// using the top gear and the optimal engine torque for the calculations
	
	dFloat topGearRatio = GetTopGear();
	dFloat tireRadio = tire->GetInfo().m_radio;
	m_info.m_crownGearRatio = tireRadio * m_info.m_rpmAtPeakHorsePower / (m_info.m_vehicleTopSpeed * topGearRatio);
}


void CustomVehicleController::EngineController::InitEngineTorqueCurve()
{
	m_info.ConvertToMetricSystem();

	CalculateCrownGear();

	dFloat rpsTable[6];
	dFloat torqueTable[6];

	rpsTable[0] = 0.0f;
	rpsTable[1] = m_info.m_rpmAtIdleTorque * 0.92f;
	rpsTable[2] = m_info.m_rpmAtPeakTorque * 0.92f;
	rpsTable[3] = m_info.m_rpmAtPeakHorsePower * 0.92f;
	rpsTable[4] = m_info.m_rpmAtReadLineTorque * 0.92f;
	rpsTable[5] = m_info.m_rpmAtReadLineTorque;

	torqueTable[0] = m_info.m_idleTorque;
	torqueTable[1] = m_info.m_idleTorque;
	torqueTable[2] = m_info.m_peakTorque;
	torqueTable[3] = m_info.m_peakPowerTorque;
	torqueTable[4] = m_info.m_redLineTorque;
	torqueTable[5] = 0.0f;

	m_torqueRPMCurve.InitalizeCurve(sizeof (rpsTable) / sizeof (rpsTable[0]), rpsTable, torqueTable);
	m_info.m_minRPM = m_info.m_rpmAtIdleTorque * D_VEHICLE_MIN_RPM_FACTOR;

	dFloat minTorque = m_torqueRPMCurve.GetValue(m_info.m_rpmAtIdleTorque) * D_VEHICLE_REST_GAS_VALVE;
	m_info.m_idleViscousDrag1 = 4.0f * minTorque / m_info.m_rpmAtIdleTorque;
	m_info.m_idleViscousDrag2 = 2.0f * minTorque / (m_info.m_rpmAtIdleTorque * m_info.m_rpmAtIdleTorque);
	m_info.m_minTorque = dMax (minTorque, m_info.m_redLineTorque * 0.9f);
}

void CustomVehicleController::EngineController::PlotEngineCurve() const
{
#ifdef D_PLOT_ENGINE_CURVE 
	dFloat rpm0 = m_torqueRPMCurve.m_nodes[0].m_param;
	dFloat rpm1 = m_torqueRPMCurve.m_nodes[m_torqueRPMCurve.m_count - 1].m_param;
	int steps = 40;
	dFloat omegaStep = (rpm1 - rpm0) / steps;
	dTrace(("rpm\ttorque\tpower\n"));
	for (int i = 0; i < steps; i++) {
		dFloat r = rpm0 + omegaStep * i;
		dFloat torque = m_torqueRPMCurve.GetValue(r);
		dFloat power = r * torque;
		const dFloat horsePowerToWatts = 735.5f;
		const dFloat rpmToRadiansPerSecunds = 0.105f;
		const dFloat poundFootToNewtonMeters = 1.356f;
		dTrace(("%6.2f\t%6.2f\t%6.2f\n", r / 0.105f, torque / 1.356f, power / 735.5f));
	}
#endif
}


dFloat CustomVehicleController::EngineController::GetGearRatio () const
{
	return m_info.m_crownGearRatio * m_info.m_gearRatios[m_currentGear];
}


void CustomVehicleController::EngineController::UpdateAutomaticGearBox(dFloat timestep)
{
//xxx
m_info.m_gearsCount = 4;

	m_gearTimer--;
	if (m_gearTimer < 0) {
		dFloat omega = m_engine->m_omega.m_x;
		switch (m_currentGear) 
		{
			case D_VEHICLE_NEUTRAL_GEAR:
			{
			   SetGear(D_VEHICLE_NEUTRAL_GEAR);
			   break;
			}

			case D_VEHICLE_REVERSE_GEAR:
			{
				SetGear(D_VEHICLE_REVERSE_GEAR);
				break;
			}

			default:
			{
				if (omega > m_info.m_rpmAtPeakHorsePower) {
					if (m_currentGear < (m_info.m_gearsCount - 1)) {
						SetGear(m_currentGear + 1);
					}
				} else if (omega < m_info.m_rpmAtPeakTorque) {
					if (m_currentGear > D_VEHICLE_FIRST_GEAR) {
						SetGear(m_currentGear - 1);
					}
				}
			}
		}
	}
}

void CustomVehicleController::EngineController::ApplyTorque(dFloat torque, dFloat timestep)
{
	m_engine->Update(this, torque, timestep);
}

void CustomVehicleController::EngineController::Update(dFloat timestep)
{
	if (m_automaticTransmissionMode) {
		UpdateAutomaticGearBox (timestep);
	}

	dFloat torque = 0.0f;
	dFloat omega = m_engine->m_omega.m_x;
	if (m_ignitionKey) {
		dFloat gasVal = dMax (m_param, D_VEHICLE_REST_GAS_VALVE);
		dFloat drag = dMin (omega * omega * m_info.m_idleViscousDrag2, m_info.m_minTorque);
		torque = m_torqueRPMCurve.GetValue(omega) * gasVal - drag;
	} else {
		 torque = - omega * m_info.m_idleViscousDrag1;
	}
	ApplyTorque(torque, timestep);
}

bool CustomVehicleController::EngineController::GetTransmissionMode() const
{
	return m_automaticTransmissionMode;
}

void CustomVehicleController::EngineController::SetIgnition(bool key)
{
	m_ignitionKey = key;
}

bool CustomVehicleController::EngineController::GetIgnition() const
{
	return m_ignitionKey;
}


void CustomVehicleController::EngineController::SetTransmissionMode(bool mode)
{
	m_automaticTransmissionMode = mode;
}

void CustomVehicleController::EngineController::SetClutchParam (dFloat cluthParam)
{
	m_clutchParam = dClamp (cluthParam, dFloat(0.0f), dFloat(1.0f));
}


int CustomVehicleController::EngineController::GetGear() const
{
	return m_currentGear;
}

void CustomVehicleController::EngineController::SetGear(int gear)
{
	m_currentGear = dClamp(gear, 0, m_info.m_gearsCount);
	m_gearTimer = 30;
}

int CustomVehicleController::EngineController::GetNeutralGear() const
{
	return D_VEHICLE_NEUTRAL_GEAR;
}

int CustomVehicleController::EngineController::GetReverseGear() const
{
	return D_VEHICLE_REVERSE_GEAR;
}

int CustomVehicleController::EngineController::GetFirstGear() const
{
	return D_VEHICLE_FIRST_GEAR;
}

int CustomVehicleController::EngineController::GetLastGear() const
{
	return m_info.m_gearsCount - 1;
}


dFloat CustomVehicleController::EngineController::GetRPM() const
{
	return m_engine->m_omega.m_x * 9.55f;
}

dFloat CustomVehicleController::EngineController::GetIdleRPM() const
{
	return m_info.m_rpmAtIdleTorque * 9.55f;
}

dFloat CustomVehicleController::EngineController::GetRedLineRPM() const
{
	return m_info.m_rpmAtReadLineTorque * 9.55f;
}

dFloat CustomVehicleController::EngineController::GetSpeed() const
{
	dMatrix matrix;
	dVector veloc(0.0f);
	NewtonBody* const chassis = m_controller->GetBody();

	NewtonBodyGetMatrix(chassis, &matrix[0][0]);
	NewtonBodyGetVelocity(chassis, &veloc[0]);

	dVector pin (matrix.RotateVector (m_controller->m_localFrame.m_front));
	return pin % veloc;
}

dFloat CustomVehicleController::EngineController::GetTopSpeed() const
{
	return m_info.m_vehicleTopSpeed;
}

CustomVehicleController::SteeringController::SteeringController (CustomVehicleController* const controller, dFloat maxAngle)
	:Controller(controller)
	,m_maxAngle(dAbs (maxAngle))
	,m_akermanWheelBaseWidth(0.0f)
	,m_akermanAxelSeparation(0.0f)
{
}

void CustomVehicleController::SteeringController::CalculateAkermanParameters(
	const BodyPartTire* const rearLeftTire, const BodyPartTire* const rearRightTire,
	const BodyPartTire* const frontLeftTire, const BodyPartTire* const frontRightTire)
{
/*
	const dMatrix& leftRearMatrix = rearLeftTire->GetLocalMatrix();
	const dMatrix& rightRearMatrix = rearRightTire->GetLocalMatrix();
	dVector rearDist(rightRearMatrix.m_posit - leftRearMatrix.m_posit);
	m_akermanWheelBaseWidth = (rearDist % leftRearMatrix.m_front) * 0.5f;

	const dMatrix& frontLeftTireMatrix = frontLeftTire->GetLocalMatrix();
	dVector akermanAxelSeparation(frontLeftTireMatrix.m_posit - leftRearMatrix.m_posit);
	m_akermanAxelSeparation = dAbs(akermanAxelSeparation % frontLeftTireMatrix.m_right);
*/
}

void CustomVehicleController::SteeringController::Update(dFloat timestep)
{
	dFloat angle = m_maxAngle * m_param;
	if ((m_akermanWheelBaseWidth == 0.0f) || (dAbs(angle) < (2.0f * 3.141592f / 180.0f))) {
		for (dList<BodyPartTire*>::dListNode* node = m_tires.GetFirst(); node; node = node->GetNext()) {
			BodyPartTire& tire = *node->GetInfo();
			tire.SetSteerAngle(angle);
		}
	} else {
		dAssert (0);
/*
		dAssert(dAbs(angle) >= (2.0f * 3.141592f / 180.0f));
		dFloat posit = m_akermanAxelSeparation / dTan(dAbs(angle));
		dFloat sign = dSign(angle);
		dFloat leftAngle = sign * dAtan2(m_akermanAxelSeparation, posit + m_akermanWheelBaseWidth);
		dFloat righAngle = sign * dAtan2(m_akermanAxelSeparation, posit - m_akermanWheelBaseWidth);
		for (dList<BodyPartTire*>::dListNode* node = m_steeringTires.GetFirst(); node; node = node->GetNext()) {
			BodyPartTire& tire = *node->GetInfo();
			tire.SetSteerAngle ((sign * tire.m_data.m_l > 0.0f) ? leftAngle : righAngle);
		}
*/	
	}
}

void CustomVehicleController::SteeringController::AddTire (CustomVehicleController::BodyPartTire* const tire)
{
	m_tires.Append(tire);
}

CustomVehicleController::BrakeController::BrakeController(CustomVehicleController* const controller, dFloat maxBrakeTorque)
	:Controller(controller)
	,m_maxTorque(maxBrakeTorque)
{
}

void CustomVehicleController::BrakeController::AddTire(BodyPartTire* const tire)
{
	m_tires.Append(tire);
}

void CustomVehicleController::BrakeController::Update(dFloat timestep)
{
	dFloat torque = m_maxTorque * m_param;
	for (dList<BodyPartTire*>::dListNode* node = m_tires.GetFirst(); node; node = node->GetNext()) {
		BodyPartTire& tire = *node->GetInfo();
		tire.SetBrakeTorque (torque);
	}
}



void CustomVehicleControllerManager::DrawSchematic (const CustomVehicleController* const controller, dFloat scale) const
{
	controller->DrawSchematic(scale);
}

void CustomVehicleControllerManager::DrawSchematicCallback (const CustomVehicleController* const controller, const char* const partName, dFloat value, int pointCount, const dVector* const lines) const
{
}


void CustomVehicleController::DrawSchematic(dFloat scale) const
{
	dMatrix projectionMatrix(dGetIdentityMatrix());
	projectionMatrix[0][0] = scale;
	projectionMatrix[1][1] = 0.0f;
	projectionMatrix[2][1] = -scale;
	projectionMatrix[2][2] = 0.0f;
	CustomVehicleControllerManager* const manager = (CustomVehicleControllerManager*)GetManager();

	dMatrix matrix0;
	dVector com(0.0f);
	dFloat arrayPtr[32][4];
	dVector* const array = (dVector*)arrayPtr;
	dFloat Ixx;
	dFloat Iyy;
	dFloat Izz;
	dFloat mass;
	NewtonBody* const chassisBody = m_chassis.GetBody();

	dFloat velocityScale = 0.125f;

	NewtonBodyGetCentreOfMass(chassisBody, &com[0]);
	NewtonBodyGetMatrix(chassisBody, &matrix0[0][0]);
	matrix0.m_posit = matrix0.TransformVector(com);

	NewtonBodyGetMassMatrix(chassisBody, &mass, &Ixx, &Iyy, &Izz);
	dMatrix chassisMatrix(GetLocalFrame() * matrix0);
	dMatrix worldToComMatrix(chassisMatrix.Inverse() * projectionMatrix);
	{
		// draw vehicle chassis
		dVector p0(D_CUSTOM_LARGE_VALUE, D_CUSTOM_LARGE_VALUE, D_CUSTOM_LARGE_VALUE, 0.0f);
		dVector p1(-D_CUSTOM_LARGE_VALUE, -D_CUSTOM_LARGE_VALUE, -D_CUSTOM_LARGE_VALUE, 0.0f);

		for (dList<BodyPartTire>::dListNode* node = m_tireList.GetFirst(); node; node = node->GetNext()) {
			dMatrix matrix;
			BodyPartTire* const tire = &node->GetInfo();
			NewtonBody* const tireBody = tire->GetBody();
			
			NewtonBodyGetMatrix(tireBody, &matrix[0][0]);
			//dMatrix matrix (tire->CalculateSteeringMatrix() * m_chassisState.GetMatrix());
			dVector p(worldToComMatrix.TransformVector(matrix.m_posit));
			p0 = dVector(dMin(p.m_x, p0.m_x), dMin(p.m_y, p0.m_y), dMin(p.m_z, p0.m_z), 1.0f);
			p1 = dVector(dMax(p.m_x, p1.m_x), dMax(p.m_y, p1.m_y), dMax(p.m_z, p1.m_z), 1.0f);
		}

		array[0] = dVector(p0.m_x, p0.m_y, p0.m_z, 1.0f);
		array[1] = dVector(p1.m_x, p0.m_y, p0.m_z, 1.0f);
		array[2] = dVector(p1.m_x, p1.m_y, p0.m_z, 1.0f);
		array[3] = dVector(p0.m_x, p1.m_y, p0.m_z, 1.0f);
		manager->DrawSchematicCallback(this, "chassis", 0, 4, array);
	}

	{
		// draw vehicle tires
		for (dList<BodyPartTire>::dListNode* node = m_tireList.GetFirst(); node; node = node->GetNext()) {
			dMatrix matrix;
			BodyPartTire* const tire = &node->GetInfo();
			dFloat width = tire->m_data.m_width * 0.5f;
			dFloat radio = tire->m_data.m_radio;
			NewtonBody* const tireBody = tire->GetBody();
			NewtonBodyGetMatrix(tireBody, &matrix[0][0]);
			matrix.m_up = chassisMatrix.m_up;
			matrix.m_right = matrix.m_front * matrix.m_up;

			array[0] = worldToComMatrix.TransformVector(matrix.TransformVector(dVector(width, 0.0f, radio, 0.0f)));
			array[1] = worldToComMatrix.TransformVector(matrix.TransformVector(dVector(width, 0.0f, -radio, 0.0f)));
			array[2] = worldToComMatrix.TransformVector(matrix.TransformVector(dVector(-width, 0.0f, -radio, 0.0f)));
			array[3] = worldToComMatrix.TransformVector(matrix.TransformVector(dVector(-width, 0.0f, radio, 0.0f)));
			manager->DrawSchematicCallback(this, "tire", 0, 4, array);
		}
	}

	{
		// draw vehicle velocity
		dVector veloc(0.0f);
		dVector omega(0.0f);

		NewtonBodyGetOmega(chassisBody, &omega[0]);
		NewtonBodyGetVelocity(chassisBody, &veloc[0]);

		dVector localVelocity(chassisMatrix.UnrotateVector(veloc));
		localVelocity.m_y = 0.0f;

		localVelocity = projectionMatrix.RotateVector(localVelocity);
		array[0] = dVector(0.0f, 0.0f, 0.0f, 0.0f);
		array[1] = localVelocity.Scale(velocityScale);
		manager->DrawSchematicCallback(this, "velocity", 0, 2, array);

		dVector localOmega(chassisMatrix.UnrotateVector(omega));
		array[0] = dVector(0.0f, 0.0f, 0.0f, 0.0f);
		array[1] = dVector(0.0f, localOmega.m_y * 10.0f, 0.0f, 0.0f);
		manager->DrawSchematicCallback(this, "omega", 0, 2, array);
	}

	{
		dFloat scale(2.0f / (mass * 10.0f));
		// draw vehicle forces
		for (dList<BodyPartTire>::dListNode* node = m_tireList.GetFirst(); node; node = node->GetNext()) {
			BodyPartTire* const tire = &node->GetInfo();
			dMatrix matrix;
			NewtonBody* const tireBody = tire->GetBody();
			NewtonBodyGetMatrix(tireBody, &matrix[0][0]);
			matrix.m_up = chassisMatrix.m_up;
			matrix.m_right = matrix.m_front * matrix.m_up;

			dVector origin(worldToComMatrix.TransformVector(matrix.m_posit));

			dVector lateralForce(chassisMatrix.UnrotateVector(GetTireLateralForce(tire)));
			lateralForce = lateralForce.Scale(-scale);
			lateralForce = projectionMatrix.RotateVector(lateralForce);

			array[0] = origin;
			array[1] = origin + lateralForce;
			manager->DrawSchematicCallback(this, "lateralForce", 0, 2, array);

			dVector longitudinalForce(chassisMatrix.UnrotateVector(GetTireLongitudinalForce(tire)));
			longitudinalForce = longitudinalForce.Scale(-scale);
			longitudinalForce = projectionMatrix.RotateVector(longitudinalForce);

			array[0] = origin;
			array[1] = origin + longitudinalForce;
			manager->DrawSchematicCallback(this, "longitudinalForce", 0, 2, array);

			dVector veloc(0.0f);
			NewtonBodyGetVelocity(tireBody, &veloc[0]);
			veloc = chassisMatrix.UnrotateVector(veloc);
			veloc.m_y = 0.0f;
			veloc = projectionMatrix.RotateVector(veloc);
			array[0] = origin;
			array[1] = origin + veloc.Scale(velocityScale);
			manager->DrawSchematicCallback(this, "tireVelocity", 0, 2, array);
		}
	}
}

CustomVehicleControllerManager::CustomVehicleControllerManager(NewtonWorld* const world, int materialCount, int* const materialsList)
	:CustomControllerManager<CustomVehicleController> (world, VEHICLE_PLUGIN_NAME)
	,m_tireMaterial(NewtonMaterialCreateGroupID(world))
{
	// create the normalized size tire shape
	m_tireShapeTemplate = NewtonCreateChamferCylinder(world, 0.5f, 1.0f, 0, NULL);
	m_tireShapeTemplateData = NewtonCollisionDataPointer(m_tireShapeTemplate);

	// create a tire material and associate with the material the vehicle new to collide 
	for (int i = 0; i < materialCount; i ++) {
		NewtonMaterialSetCallbackUserData (world, m_tireMaterial, materialsList[i], this);
		if (m_tireMaterial != materialsList[i]) {
			NewtonMaterialSetContactGenerationCallback (world, m_tireMaterial, materialsList[i], OnContactGeneration);
		}
		NewtonMaterialSetCollisionCallback(world, m_tireMaterial, materialsList[i], OnTireAABBOverlap, OnTireContactsProcess);
	}
}

CustomVehicleControllerManager::~CustomVehicleControllerManager()
{
	NewtonDestroyCollision(m_tireShapeTemplate);
}

void CustomVehicleControllerManager::DestroyController(CustomVehicleController* const controller)
{
	controller->Cleanup();
	CustomControllerManager<CustomVehicleController>::DestroyController(controller);
}

int CustomVehicleControllerManager::GetTireMaterial() const
{
	return m_tireMaterial;
}

CustomVehicleController* CustomVehicleControllerManager::CreateVehicle(NewtonBody* const body, const dMatrix& vehicleFrame, NewtonApplyForceAndTorque forceAndTorque, void* const userData)
{
	dAssert (0);
//	CustomVehicleController* const controller = CreateController();
//	controller->Init(body, vehicleFrame, gravityVector);
//	return controller;
return NULL;	
}

CustomVehicleController* CustomVehicleControllerManager::CreateVehicle(NewtonCollision* const chassisShape, const dMatrix& vehicleFrame, dFloat mass, NewtonApplyForceAndTorque forceAndTorque, void* const userData)
{
	CustomVehicleController* const controller = CreateController();
	controller->Init(chassisShape, vehicleFrame, mass, forceAndTorque, userData);
	return controller;
}

void CustomVehicleController::Init(NewtonCollision* const chassisShape, const dMatrix& vehicleFrame, dFloat mass, NewtonApplyForceAndTorque forceAndTorque, void* const userData)
{
	CustomVehicleControllerManager* const manager = (CustomVehicleControllerManager*)GetManager();
	NewtonWorld* const world = manager->GetWorld();

	// create a body and an call the low level init function
	m_isAirborned = false;
	m_hasNewContact = false;
	m_lastAngularMomentum = dVector (0.0f);
	dMatrix locationMatrix(dGetIdentityMatrix());
	NewtonBody* const body = NewtonCreateDynamicBody(world, chassisShape, &locationMatrix[0][0]);

	// set vehicle mass, inertia and center of mass
	NewtonBodySetMassProperties(body, mass, chassisShape);

	// initialize 
	Init(body, vehicleFrame, forceAndTorque, userData);
}

void CustomVehicleController::Init(NewtonBody* const body, const dMatrix& vehicleFrame, NewtonApplyForceAndTorque forceAndTorque, void* const userData)
{
	m_body = body;
	m_finalized = false;
	m_weightDistribution = 0.5f;
	m_localFrame = vehicleFrame;
	m_localFrame.m_posit = dVector (0.0f, 0.0f, 0.0f, 1.0f);
	m_forceAndTorque = forceAndTorque;
	
	CustomVehicleControllerManager* const manager = (CustomVehicleControllerManager*)GetManager();
	NewtonWorld* const world = manager->GetWorld();

	// set linear and angular drag to zero
	dVector drag(0.0f, 0.0f, 0.0f, 0.0f);
	NewtonBodySetLinearDamping(m_body, 0);
	NewtonBodySetAngularDamping(m_body, &drag[0]);

	// set the standard force and torque call back
	NewtonBodySetForceAndTorqueCallback(body, m_forceAndTorque);

	m_contactFilter = new BodyPartTire::FrictionModel(this);

//	SetDryRollingFrictionTorque(100.0f / 4.0f);
	m_brakesControl = NULL;
	m_engineControl = NULL;
	m_handBrakesControl = NULL;
	m_steeringControl = NULL;
	
	m_collisionAggregate = NewtonCollisionAggregateCreate(world);
	NewtonCollisionAggregateSetSelfCollision (m_collisionAggregate, 0);
	NewtonCollisionAggregateAddBody (m_collisionAggregate, m_body);

	m_skeleton = NewtonSkeletonContainerCreate(world, m_body, NULL);

	m_chassis.Init(this, userData);
	m_bodyPartsList.Append(&m_chassis);

#ifdef D_PLOT_ENGINE_CURVE 
	file_xxx = fopen("vehiceLog.csv", "wb");
	fprintf (file_xxx, "eng_rpm, eng_torque, eng_nominalTorque,\n");
#endif
}

void CustomVehicleController::Cleanup()
{
	SetBrakes(NULL);
	SetEngine(NULL);
	SetSteering(NULL);
	SetHandBrakes(NULL);
	SetContactFilter(NULL);
}

const CustomVehicleController::BodyPart* CustomVehicleController::GetChassis() const
{
	return &m_chassis;
}

const dMatrix& CustomVehicleController::GetLocalFrame() const
{
	return m_localFrame;
}

dMatrix CustomVehicleController::GetTransform() const
{
	dMatrix matrix;
	NewtonBodyGetMatrix (m_chassis.GetBody(), &matrix[0][0]);
	return matrix;
}

void CustomVehicleController::SetTransform(const dMatrix& matrix)
{
	NewtonBodySetMatrixRecursive (m_chassis.GetBody(), &matrix[0][0]);
}


CustomVehicleController::EngineController* CustomVehicleController::GetEngine() const
{
	return m_engineControl;
}


CustomVehicleController::SteeringController* CustomVehicleController::GetSteering() const
{
	return m_steeringControl;
}

CustomVehicleController::BrakeController* CustomVehicleController::GetBrakes() const
{
	return m_brakesControl;
}

CustomVehicleController::BrakeController* CustomVehicleController::GetHandBrakes() const
{
	return m_handBrakesControl;
}

void CustomVehicleController::SetEngine(EngineController* const engineControl)
{
	if (m_engineControl) {
		delete m_engineControl;
	}
	m_engineControl = engineControl;
}


void CustomVehicleController::SetHandBrakes(BrakeController* const handBrakes)
{
	if (m_handBrakesControl) {
		delete m_handBrakesControl;
	}
	m_handBrakesControl = handBrakes;
}

void CustomVehicleController::SetBrakes(BrakeController* const brakes)
{
	if (m_brakesControl) {
		delete m_brakesControl;
	}
	m_brakesControl = brakes;
}


void CustomVehicleController::SetSteering(SteeringController* const steering)
{
	if (m_steeringControl) {
		delete m_steeringControl;
	}
	m_steeringControl = steering;
}

void CustomVehicleController::SetContactFilter(BodyPartTire::FrictionModel* const filter)
{
	if (m_contactFilter) {
		delete m_contactFilter;
	}
	m_contactFilter = filter;
}

dList<CustomVehicleController::BodyPartTire>::dListNode* CustomVehicleController::GetFirstTire() const
{
	return m_tireList.GetFirst();
}

dList<CustomVehicleController::BodyPartTire>::dListNode* CustomVehicleController::GetNextTire(dList<CustomVehicleController::BodyPartTire>::dListNode* const tireNode) const
{
	return tireNode->GetNext();
}

dList<CustomVehicleController::BodyPart*>::dListNode* CustomVehicleController::GetFirstBodyPart() const
{
	return m_bodyPartsList.GetFirst();
}

dList<CustomVehicleController::BodyPart*>::dListNode* CustomVehicleController::GetNextBodyPart(dList<BodyPart*>::dListNode* const part) const
{
	return part->GetNext();
}

void CustomVehicleController::SetCenterOfGravity(const dVector& comRelativeToGeomtriCenter)
{
	NewtonBodySetCentreOfMass(m_body, &comRelativeToGeomtriCenter[0]);
}

dFloat CustomVehicleController::GetWeightDistribution() const
{
	return m_weightDistribution;
}

void CustomVehicleController::SetWeightDistribution(dFloat weightDistribution)
{
	m_weightDistribution = dClamp (weightDistribution, dFloat(0.0f), dFloat(1.0f));
	if (m_finalized) {
		dFloat factor = m_weightDistribution - 0.5f;

		dMatrix matrix;
		dMatrix tireMatrix;
		dVector origin(0.0f);
		dVector totalMassOrigin (0.0f);
		dFloat xMin = 1.0e10f;
		dFloat xMax = -1.0e10f;
		dFloat totalMass;
		dFloat Ixx;
		dFloat Iyy;
		dFloat Izz;

		NewtonBodyGetMatrix(m_body, &matrix[0][0]);
		NewtonBodyGetCentreOfMass(m_body, &origin[0]);
		NewtonBodyGetMassMatrix (m_body, &totalMass, &Ixx, &Iyy, &Izz);
		totalMassOrigin = origin.Scale (totalMass);

		matrix = matrix.Inverse();
		for (dList<BodyPartTire>::dListNode* node = m_tireList.GetFirst(); node; node = node->GetNext()) {
			dFloat mass;
			BodyPartTire* const tire = &node->GetInfo();
			NewtonBodyGetMatrix(tire->GetBody(), &tireMatrix[0][0]);
			NewtonBodyGetMassMatrix (tire->GetBody(), &mass, &Ixx, &Iyy, &Izz);
			tireMatrix = tireMatrix * matrix;
			totalMassOrigin += tireMatrix.m_posit.Scale (mass);
			totalMass += mass;
			xMin = dMin (xMin, tireMatrix.m_posit.m_x); 
			xMax = dMax (xMax, tireMatrix.m_posit.m_x); 
		}
		origin = totalMassOrigin.Scale (1.0f / totalMass);

		dVector vehCom (0.0f);
		NewtonBodyGetCentreOfMass(m_body, &vehCom[0]);
		vehCom.m_x = origin.m_x + (xMax - xMin) * factor;
		vehCom.m_z = origin.m_z;
		NewtonBodySetCentreOfMass(m_body, &vehCom[0]);


/*
		dWeightDistibutionSolver solver;
		dFloat64 unitAccel[256];
		dFloat64 sprungMass[256];

		int count = 0;
		dVector dir(0.0f, 1.0f, 0.0f, 0.0f);

		dMatrix matrix;
		dVector com(0.0f);
		dVector invInertia(0.0f);
		dFloat invMass;
		NewtonBodyGetMatrix(m_body, &matrix[0][0]);
		NewtonBodyGetCentreOfMass(m_body, &com[0]);
		NewtonBodyGetInvMass(m_body, &invMass, &invInertia[0], &invInertia[1], &invInertia[2]);
		matrix = matrix.Inverse();

		for (dList<BodyPartTire>::dListNode* node = m_tireList.GetFirst(); node; node = node->GetNext()) {
			BodyPartTire* const tire = &node->GetInfo();

			dMatrix tireMatrix;
			NewtonBodyGetMatrix(tire->GetBody(), &tireMatrix[0][0]);
			tireMatrix = tireMatrix * matrix;

			dVector posit(tireMatrix.m_posit - com);

			dComplentaritySolver::dJacobian &jacobian0 = solver.m_jacobians[count];
			dComplentaritySolver::dJacobian &invMassJacobian0 = solver.m_invMassJacobians[count];
			jacobian0.m_linear = dir;
			jacobian0.m_angular = posit * dir;
			jacobian0.m_angular.m_w = 0.0f;

			invMassJacobian0.m_linear = jacobian0.m_linear.Scale(invMass);
			invMassJacobian0.m_angular = jacobian0.m_angular.CompProduct(invInertia);

			dFloat diagonal = jacobian0.m_linear % invMassJacobian0.m_linear + jacobian0.m_angular % invMassJacobian0.m_angular;
			solver.m_diagRegularizer[count] = diagonal * 0.005f;
			solver.m_invDiag[count] = 1.0f / (diagonal + solver.m_diagRegularizer[count]);

			unitAccel[count] = 1.0f;
			sprungMass[count] = 0.0f;
			count++;
		}

		if (count) {
			solver.m_count = count;
			solver.Solve(count, 1.0e-6f, sprungMass, unitAccel);
		}


		int index = 0;
		for (dList<BodyPartTire>::dListNode* node = m_tireList.GetFirst(); node; node = node->GetNext()) {
			BodyPartTire* const tire = &node->GetInfo();
			WheelJoint* const tireJoint = (WheelJoint*)tire->GetJoint();
			tireJoint->m_restSprunMass = dFloat(5.0f * dFloor(sprungMass[index] / 5.0f + 0.5f));
			index++;
		}
*/
	}

}


void CustomVehicleController::Finalize()
{
//NewtonBodySetMassMatrix(GetBody(), 0.0f, 0.0f, 0.0f, 0.0f);
	m_finalized = true;
	NewtonSkeletonContainerFinalize(m_skeleton);
	SetWeightDistribution (m_weightDistribution);
}

bool CustomVehicleController::ControlStateChanged() const
{
	bool inputChanged = (m_steeringControl && m_steeringControl->ParamChanged());
	inputChanged = inputChanged || (m_engineControl && m_engineControl ->ParamChanged());
	inputChanged = inputChanged || (m_brakesControl && m_brakesControl->ParamChanged());
	inputChanged = inputChanged || (m_handBrakesControl && m_handBrakesControl->ParamChanged());
	inputChanged = inputChanged || m_hasNewContact;
	return inputChanged;
}

CustomVehicleController::BodyPartTire* CustomVehicleController::AddTire(const BodyPartTire::Info& tireInfo)
{
	dList<BodyPartTire>::dListNode* const tireNode = m_tireList.Append();
	BodyPartTire& tire = tireNode->GetInfo();

	// calculate the tire matrix location,
	dMatrix matrix;
	NewtonBodyGetMatrix(m_body, &matrix[0][0]);
	matrix = m_localFrame * matrix;
	matrix.m_posit = matrix.TransformVector (tireInfo.m_location);
	matrix.m_posit.m_w = 1.0f;

	tire.Init(&m_chassis, matrix, tireInfo);
	tire.m_index = m_tireList.GetCount() - 1;

	m_bodyPartsList.Append(&tire);
	NewtonCollisionAggregateAddBody (m_collisionAggregate, tire.GetBody());
	NewtonSkeletonContainerAttachBone (m_skeleton, tire.GetBody(), m_chassis.GetBody());
	return &tireNode->GetInfo();
}


dVector CustomVehicleController::GetTireNormalForce(const BodyPartTire* const tire) const
{
	WheelJoint* const joint = (WheelJoint*) tire->GetJoint();
	dFloat force = joint->GetTireLoad();
	return dVector (0.0f, force, 0.0f, 0.0f);
}

dVector CustomVehicleController::GetTireLateralForce(const BodyPartTire* const tire) const
{
	WheelJoint* const joint = (WheelJoint*)tire->GetJoint();
	return joint->GetLateralForce();
}

dVector CustomVehicleController::GetTireLongitudinalForce(const BodyPartTire* const tire) const
{
	WheelJoint* const joint = (WheelJoint*)tire->GetJoint();
	return joint->GetLongitudinalForce();
}

dFloat CustomVehicleController::GetAerodynamicsDowforceCoeficient() const
{
	return m_chassis.m_aerodynamicsDownForceCoefficient;
}

void CustomVehicleController::SetAerodynamicsDownforceCoefficient(dFloat maxDownforceInGravity, dFloat downWeightRatioAtSpeedFactor, dFloat speedFactor, dFloat maxWeightAtTopSpeed)
{
	dFloat Ixx;
	dFloat Iyy;
	dFloat Izz;
	dFloat mass;

	dAssert (speedFactor >= 0.0f);
	dAssert (speedFactor <= 1.0f);
	dAssert (downWeightRatioAtSpeedFactor >= 0.0f);
	dAssert (downWeightRatioAtSpeedFactor < maxWeightAtTopSpeed);
	NewtonBody* const body = GetBody();
	NewtonBodyGetMassMatrix(body, &mass, &Ixx, &Iyy, &Izz);
	dFloat topSpeed = m_engineControl ? m_engineControl->GetTopSpeed() : 25.0f;
	m_chassis.m_aerodynamicsDownSpeedCutOff = topSpeed * speedFactor;
	m_chassis.m_aerodynamicsDownForce0 = mass * downWeightRatioAtSpeedFactor * dAbs (maxDownforceInGravity);
	m_chassis.m_aerodynamicsDownForce1 = mass * maxWeightAtTopSpeed * dAbs (maxDownforceInGravity);
	m_chassis.m_aerodynamicsDownForceCoefficient = m_chassis.m_aerodynamicsDownForce0 / (m_chassis.m_aerodynamicsDownSpeedCutOff * m_chassis.m_aerodynamicsDownSpeedCutOff);
}

int CustomVehicleControllerManager::OnTireAABBOverlap(const NewtonMaterial* const material, const NewtonBody* const body0, const NewtonBody* const body1, int threadIndex)
{
	CustomVehicleControllerManager* const manager = (CustomVehicleControllerManager*)NewtonMaterialGetMaterialPairUserData(material);

	const NewtonCollision* const collision0 = NewtonBodyGetCollision(body0);
	const void* const data0 = NewtonCollisionDataPointer(collision0);
	if (data0 == manager->m_tireShapeTemplateData) {
		const NewtonBody* const otherBody = body1;
		const CustomVehicleController::BodyPartTire* const tire = (CustomVehicleController::BodyPartTire*) NewtonCollisionGetUserData1(collision0);
		dAssert(tire->GetParent()->GetBody() != otherBody);
		return manager->OnTireAABBOverlap(material, tire, otherBody);
	} 
	const NewtonCollision* const collision1 = NewtonBodyGetCollision(body1);
	dAssert (NewtonCollisionDataPointer(collision1) == manager->m_tireShapeTemplateData) ;
	const NewtonBody* const otherBody = body0;
	const CustomVehicleController::BodyPartTire* const tire = (CustomVehicleController::BodyPartTire*) NewtonCollisionGetUserData1(collision1);
	dAssert(tire->GetParent()->GetBody() != otherBody);
	return manager->OnTireAABBOverlap(material, tire, otherBody);
}

int CustomVehicleControllerManager::OnTireAABBOverlap(const NewtonMaterial* const material, const CustomVehicleController::BodyPartTire* const tire, const NewtonBody* const otherBody) const
{
	for (int i = 0; i < tire->m_collidingCount; i ++) {
		if (otherBody == tire->m_contactInfo[i].m_hitBody) {
			return 1;
		}
	}
	tire->GetController()->m_hasNewContact |= tire->m_data.m_hasFender ? false : true;
	return tire->m_data.m_hasFender ? 0 : 1;
}

int CustomVehicleControllerManager::OnContactGeneration (const NewtonMaterial* const material, const NewtonBody* const body0, const NewtonCollision* const collision0, const NewtonBody* const body1, const NewtonCollision* const collision1, NewtonUserContactPoint* const contactBuffer, int maxCount, int threadIndex)
{
	CustomVehicleControllerManager* const manager = (CustomVehicleControllerManager*) NewtonMaterialGetMaterialPairUserData(material);
	const void* const data0 = NewtonCollisionDataPointer(collision0);
//	const void* const data1 = NewtonCollisionDataPointer(collision1);
	dAssert ((data0 == manager->m_tireShapeTemplateData) || (NewtonCollisionDataPointer(collision1) == manager->m_tireShapeTemplateData));
	dAssert (!((data0 == manager->m_tireShapeTemplateData) && (NewtonCollisionDataPointer(collision1) == manager->m_tireShapeTemplateData)));

	if (data0 == manager->m_tireShapeTemplateData) {
		const NewtonBody* const otherBody = body1;
		const NewtonCollision* const tireCollision = collision0;
		const NewtonCollision* const otherCollision = collision1;
		const CustomVehicleController::BodyPartTire* const tire = (CustomVehicleController::BodyPartTire*) NewtonCollisionGetUserData1(tireCollision);
		dAssert (tire->GetBody() == body0);
		return manager->OnContactGeneration (tire, otherBody, otherCollision, contactBuffer, maxCount, threadIndex);
	} 
	dAssert (NewtonCollisionDataPointer(collision1) == manager->m_tireShapeTemplateData);
	const NewtonBody* const otherBody = body0;
	const NewtonCollision* const tireCollision = collision1;
	const NewtonCollision* const otherCollision = collision0;
	const CustomVehicleController::BodyPartTire* const tire = (CustomVehicleController::BodyPartTire*) NewtonCollisionGetUserData1(tireCollision);
	dAssert (tire->GetBody() == body1);
	int count = manager->OnContactGeneration(tire, otherBody, otherCollision, contactBuffer, maxCount, threadIndex);

	for (int i = 0; i < count; i ++) {	
		contactBuffer[i].m_normal[0] *= -1.0f;
		contactBuffer[i].m_normal[1] *= -1.0f;
		contactBuffer[i].m_normal[2] *= -1.0f;
		dSwap (contactBuffer[i].m_shapeId0, contactBuffer[i].m_shapeId1);
	}
	return count;
}


bool CustomVehicleControllerManager::Collide(CustomVehicleController::BodyPartTire* const tire, int threadIndex) const
{
	dMatrix tireMatrix;
	dMatrix chassisMatrix;

	const NewtonWorld* const world = GetWorld();
	const NewtonBody* const tireBody = tire->GetBody();
	const NewtonBody* const vehicleBody = tire->GetParent()->GetBody();
	CustomVehicleController* const controller = tire->GetController();

	NewtonBodyGetMatrix(tireBody, &tireMatrix[0][0]);
	NewtonBodyGetMatrix(vehicleBody, &chassisMatrix[0][0]);

	chassisMatrix = controller->m_localFrame * chassisMatrix;
	chassisMatrix.m_posit = chassisMatrix.TransformVector(tire->m_data.m_location);
	chassisMatrix.m_posit.m_w = 1.0f;

	dVector suspensionSpan (chassisMatrix.m_up.Scale(tire->m_data.m_suspesionlenght));

	dMatrix tireSweeptMatrix;
	tireSweeptMatrix.m_up = chassisMatrix.m_up;
	tireSweeptMatrix.m_right = tireMatrix.m_front * chassisMatrix.m_up;
	tireSweeptMatrix.m_right = tireSweeptMatrix.m_right.Scale(1.0f / dSqrt(tireSweeptMatrix.m_right % tireSweeptMatrix.m_right));
	tireSweeptMatrix.m_front = tireSweeptMatrix.m_up * tireSweeptMatrix.m_right;
	tireSweeptMatrix.m_posit = chassisMatrix.m_posit + suspensionSpan;

	NewtonCollision* const tireCollision = NewtonBodyGetCollision(tireBody);
	TireFilter filter(tire, vehicleBody);

	dFloat timeOfImpact;
	tire->m_collidingCount = 0;
	const int maxContactCount = 2;
	dAssert (sizeof (tire->m_contactInfo) / sizeof (tire->m_contactInfo[0]) > 2);
	int count = NewtonWorldConvexCast (world, &tireSweeptMatrix[0][0], &chassisMatrix.m_posit[0], tireCollision, &timeOfImpact, &filter, CustomControllerConvexCastPreFilter::Prefilter, tire->m_contactInfo, maxContactCount, threadIndex);
	if (count) {
		timeOfImpact = 1.0f - timeOfImpact;
		dFloat num = (tireMatrix.m_posit - chassisMatrix.m_up.Scale (0.25f * tire->m_data.m_suspesionlenght) - chassisMatrix.m_posit) % suspensionSpan;
		dFloat tireParam = num / (tire->m_data.m_suspesionlenght * tire->m_data.m_suspesionlenght);

		if (tireParam <= timeOfImpact) {
			tireSweeptMatrix.m_posit = chassisMatrix.m_posit + chassisMatrix.m_up.Scale(timeOfImpact * tire->m_data.m_suspesionlenght);
			for (int i = count - 1; i >= 0; i --) {
				dVector p (tireSweeptMatrix.UntransformVector (dVector (tire->m_contactInfo[i].m_point[0], tire->m_contactInfo[i].m_point[1], tire->m_contactInfo[i].m_point[2], 1.0f)));
				dFloat tangAngle = dAbs (p.m_x / p.m_y) ;
				if (tangAngle > 0.4f) {
					tire->m_contactInfo[i] = tire->m_contactInfo[count - 1];
					count --;
				}
			}
			if (count) {
				tireMatrix.m_posit = chassisMatrix.m_posit + chassisMatrix.m_up.Scale(timeOfImpact * tire->m_data.m_suspesionlenght);
				NewtonBodySetMatrixNoSleep(tireBody, &tireMatrix[0][0]);
			}
		} else {
			count = 0;
		}
	}

	tire->m_collidingCount = count;
	if (!tire->m_data.m_hasFender) {
		count = NewtonWorldCollide (world, &tireMatrix[0][0], tireCollision, &filter, CustomControllerConvexCastPreFilter::Prefilter, &tire->m_contactInfo[count], maxContactCount, threadIndex);
		for (int i = 0; i < count; i++) {
			if (tire->m_contactInfo[tire->m_collidingCount + i].m_penetration == 0.0f) {
				tire->m_contactInfo[tire->m_collidingCount + i].m_penetration = 1.0e-5f;
			}
		}
		tire->m_collidingCount += count;
	}

	return tire->m_collidingCount ? true : false;
}


void CustomVehicleControllerManager::OnTireContactsProcess (const NewtonJoint* const contactJoint, dFloat timestep, int threadIndex)
{
	void* const contact = NewtonContactJointGetFirstContact(contactJoint);
	dAssert (contact);
	NewtonMaterial* const material = NewtonContactGetMaterial(contact);
	CustomVehicleControllerManager* const manager = (CustomVehicleControllerManager*) NewtonMaterialGetMaterialPairUserData(material);

	const NewtonBody* const body0 = NewtonJointGetBody0(contactJoint);
	const NewtonBody* const body1 = NewtonJointGetBody1(contactJoint);
	const NewtonCollision* const collision0 = NewtonBodyGetCollision(body0);
	const void* const data0 = NewtonCollisionDataPointer(collision0);
	if (data0 == manager->m_tireShapeTemplateData) {
		const NewtonBody* const otherBody = body1;
		CustomVehicleController::BodyPartTire* const tire = (CustomVehicleController::BodyPartTire*) NewtonCollisionGetUserData1(collision0);
		dAssert(tire->GetParent()->GetBody() != otherBody);
		manager->OnTireContactsProcess(contactJoint, tire, otherBody, timestep);
	} else {
		const NewtonCollision* const collision1 = NewtonBodyGetCollision(body1);
		const void* const data1 = NewtonCollisionDataPointer(collision1);
		if (data1 == manager->m_tireShapeTemplateData) {
			const NewtonCollision* const collision1 = NewtonBodyGetCollision(body1);
			dAssert(NewtonCollisionDataPointer(collision1) == manager->m_tireShapeTemplateData);
			const NewtonBody* const otherBody = body0;
			CustomVehicleController::BodyPartTire* const tire = (CustomVehicleController::BodyPartTire*) NewtonCollisionGetUserData1(collision1);
			dAssert(tire->GetParent()->GetBody() != otherBody);
			manager->OnTireContactsProcess(contactJoint, tire, otherBody, timestep);
		}
	}
}


int CustomVehicleControllerManager::OnContactGeneration(const CustomVehicleController::BodyPartTire* const tire, const NewtonBody* const otherBody, const NewtonCollision* const othercollision, NewtonUserContactPoint* const contactBuffer, int maxCount, int threadIndex) const
{
	int count = 0;
	NewtonCollision* const collisionA = NewtonBodyGetCollision(tire->GetBody());
	dLong tireID = NewtonCollisionGetUserID(collisionA);
	for (int i = 0; i < tire->m_collidingCount; i++) {
		if (otherBody == tire->m_contactInfo[i].m_hitBody) {
			contactBuffer[count].m_point[0] = tire->m_contactInfo[i].m_point[0];
			contactBuffer[count].m_point[1] = tire->m_contactInfo[i].m_point[1];
			contactBuffer[count].m_point[2] = tire->m_contactInfo[i].m_point[2];
			contactBuffer[count].m_point[3] = 1.0f;
			contactBuffer[count].m_normal[0] = tire->m_contactInfo[i].m_normal[0];
			contactBuffer[count].m_normal[1] = tire->m_contactInfo[i].m_normal[1];
			contactBuffer[count].m_normal[2] = tire->m_contactInfo[i].m_normal[2];
			contactBuffer[count].m_normal[3] = 0.0f;
			contactBuffer[count].m_penetration = tire->m_contactInfo[i].m_penetration;
			contactBuffer[count].m_shapeId0 = tireID;
			contactBuffer[count].m_shapeId1 = tire->m_contactInfo[i].m_contactID;
			count++;
		}
	}
	return count;
}

void CustomVehicleControllerManager::OnTireContactsProcess(const NewtonJoint* const contactJoint, CustomVehicleController::BodyPartTire* const tire, const NewtonBody* const otherBody, dFloat timestep)
{
	dAssert((tire->GetBody() == NewtonJointGetBody0(contactJoint)) || (tire->GetBody() == NewtonJointGetBody1(contactJoint)));

	dMatrix tireMatrix;
	dVector tireOmega(0.0f);
	dVector tireVeloc(0.0f);

	NewtonBody* const tireBody = tire->GetBody();
	const CustomVehicleController* const controller = tire->GetController();
	CustomVehicleController::WheelJoint* const tireJoint = (CustomVehicleController::WheelJoint*) tire->GetJoint();

	dAssert(tireJoint->GetBody0() == tireBody);
	NewtonBodyGetMatrix(tireBody, &tireMatrix[0][0]);
	tireMatrix = tireJoint->GetMatrix0() * tireMatrix;

	NewtonBodyGetOmega(tireBody, &tireOmega[0]);
	NewtonBodyGetVelocity(tireBody, &tireVeloc[0]);

	tire->m_lateralSlip = 0.0f;
	tire->m_aligningTorque = 0.0f;
	tire->m_longitudinalSlip = 0.0f;
	tire->m_driveTorque = 0.0f;

	for (void* contact = NewtonContactJointGetFirstContact(contactJoint); contact; contact = NewtonContactJointGetNextContact(contactJoint, contact)) {
		NewtonMaterial* const material = NewtonContactGetMaterial(contact);
		if (NewtonMaterialGetContactPenetration (material) > 0.0f) {
			NewtonMaterialSetContactFrictionCoef(material, 1.0f, 1.0f, 0);
			NewtonMaterialSetContactFrictionCoef(material, 1.0f, 1.0f, 1);
		} else {
			dVector contactPoint(0.0f);
			dVector contactNormal(0.0f);
			NewtonMaterialGetContactPositionAndNormal(material, tireBody, &contactPoint[0], &contactNormal[0]);
			const dVector& lateralPin = tireMatrix.m_front;
			dVector tireAnglePin(contactNormal * lateralPin);
			dFloat pinMag2 = tireAnglePin % tireAnglePin;
			if (pinMag2 > 0.25f) {
				// brush rubber tire friction model
				// project the contact point to the surface of the collision shape
				dVector contactPatch(contactPoint - lateralPin.Scale((contactPoint - tireMatrix.m_posit) % lateralPin));
				dVector dp(contactPatch - tireMatrix.m_posit);
				dVector radius(dp.Scale(tire->m_data.m_radio / dSqrt(dp % dp)));

				dVector lateralContactDir(0.0f);
				dVector longitudinalContactDir(0.0f);
				NewtonMaterialContactRotateTangentDirections(material, &lateralPin[0]);
				NewtonMaterialGetContactTangentDirections(material, tireBody, &lateralContactDir[0], &longitudinalContactDir[0]);

				//dFloat tireOriginLateralSpeed = tireVeloc % lateralPin;
				dFloat tireOriginLateralSpeed = tireVeloc % longitudinalContactDir;
				dFloat tireOriginLongitudinalSpeed = tireVeloc % longitudinalContactDir;
				dFloat tireContactLongitudinalSpeed = -((tireOmega * radius) % longitudinalContactDir);

				if ((dAbs(tireOriginLongitudinalSpeed) < (1.0f)) || (dAbs(tireContactLongitudinalSpeed) < 0.1f)) {
					// vehicle  moving at speed for which tire physics is undefined, simple do a kinematic motion
					NewtonMaterialSetContactFrictionCoef(material, 1.0f, 1.0f, 0);
					NewtonMaterialSetContactFrictionCoef(material, 1.0f, 1.0f, 1);
				} else {
					// calculating Brush tire model with longitudinal and lateral coupling 
					// for friction coupling according to Motor Vehicle dynamics by: Giancarlo Genta 
					// reduces to this, which may have a divide by zero locked, so I am cl;amping to some small value
					// dFloat alphaTangent = vy / dAbs(vx);
					//dFloat k = (vw - vx) / vx;
					//dFloat phy_x0 = k / (1.0f + k);
					//dFloat phy_y0 = alphaTangent / (1.0f + k);
					if (dAbs(tireContactLongitudinalSpeed) < 0.01f) {
						tireContactLongitudinalSpeed = 0.01f * dSign(tireContactLongitudinalSpeed);
					}

					dFloat lateralSideSlip = tireOriginLateralSpeed / dAbs (tireContactLongitudinalSpeed);
					dFloat longitudinalSlipRatio = (tireContactLongitudinalSpeed - tireOriginLongitudinalSpeed) / tireContactLongitudinalSpeed;

					dFloat aligningMoment;
					dFloat lateralForce;
					dFloat longitudinalForce;

					dVector tireLoadForce(0.0f);
					NewtonMaterialGetContactForce(material, tireBody, &tireLoadForce.m_x);
					dFloat tireLoad = (tireLoadForce % contactNormal);

					controller->m_contactFilter->GetForces(tire, otherBody, material, tireLoad, longitudinalSlipRatio, lateralSideSlip, longitudinalForce, lateralForce, aligningMoment);

					//dVector force (longitudinalContactDir.Scale (f_x) + lateralPin.Scale (f_z));
					//dVector force (longitudinalContactDir.Scale (f_x) + lateralContactDir.Scale (f_z));
					//dVector torque (radius * force);

					//NewtonBodyAddForce(tireBody, &force[0]);
					//NewtonBodyAddForce(tireBody, &torque[0]);
					dFloat sign = dSign (tireContactLongitudinalSpeed - tireOriginLongitudinalSpeed);
					tire->m_driveTorque -= longitudinalForce * sign * tire->m_data.m_radio;
				
					NewtonMaterialSetContactTangentAcceleration (material, 0.0f, 0);
					NewtonMaterialSetContactTangentFriction(material, dAbs (lateralForce), 0);

					NewtonMaterialSetContactTangentAcceleration (material, 0.0f, 1);
					NewtonMaterialSetContactTangentFriction(material, dAbs (longitudinalForce), 1);

	//dTrace (("(%d %f) ", tire->m_index, tire->m_powerTorque));
				}

			} else {
				NewtonMaterialSetContactFrictionCoef(material, 1.0f, 1.0f, 0);
				NewtonMaterialSetContactFrictionCoef(material, 1.0f, 1.0f, 1);
			}
		}

		NewtonMaterialSetContactElasticity(material, 0.0f);
	}
}




void CustomVehicleController::ApplyLateralStabilityForces(dFloat timestep)
{
//	CustomVehicleControllerManager* const manager = (CustomVehicleControllerManager*)GetManager();

	dMatrix matrix0;
	
	dVector veloc(0.0f);
	dVector com(0.0f);
//	dFloat arrayPtr[32][4];
//	dVector* const array = (dVector*)arrayPtr;
//	dFloat Ixx;
//	dFloat Iyy;
//	dFloat Izz;
//	dFloat mass;
	NewtonBody* const chassisBody = m_chassis.GetBody();
	NewtonBodyGetCentreOfMass(chassisBody, &com[0]);
	NewtonBodyGetMatrix(chassisBody, &matrix0[0][0]);
	matrix0.m_posit = matrix0.TransformVector(com);

	dMatrix chassisMatrix(GetLocalFrame() * matrix0);
	NewtonBodyGetVelocity(chassisBody, &veloc[0]);

	veloc = chassisMatrix.UnrotateVector(veloc);
	dVector sideSlipDir (veloc - chassisMatrix.m_up.Scale (veloc % chassisMatrix.m_up));
	veloc.m_y = 0.0f;
	dFloat mag2 = veloc % veloc;
	if (mag2 > 1.0f) {
		sideSlipDir = veloc.Scale (1.0f / dSqrt (veloc % veloc));
//		dFloat speed = sideSlipDir % veloc;

		dVector omega(0.0f);
		NewtonBodyGetOmega(chassisBody, &omega[0]);	
		omega = chassisMatrix.UnrotateVector(omega);
//		dFloat yawRate = omega.m_y;

//		dTrace (("%f %f\n", speed, yawRate));
	}
/*
	NewtonBodyGetMassMatrix(chassisBody, &mass, &Ixx, &Iyy, &Izz);
	dMatrix worldToComMatrix(chassisMatrix.Inverse() * projectionMatrix);
	{
		// draw vehicle chassis
		dVector p0(D_CUSTOM_LARGE_VALUE, D_CUSTOM_LARGE_VALUE, D_CUSTOM_LARGE_VALUE, 0.0f);
		dVector p1(-D_CUSTOM_LARGE_VALUE, -D_CUSTOM_LARGE_VALUE, -D_CUSTOM_LARGE_VALUE, 0.0f);

		for (dList<BodyPartTire>::dListNode* node = m_tireList.GetFirst(); node; node = node->GetNext()) {
			dMatrix matrix;
			BodyPartTire* const tire = &node->GetInfo();
			NewtonBody* const tireBody = tire->GetBody();
			
			NewtonBodyGetMatrix(tireBody, &matrix[0][0]);
			//dMatrix matrix (tire->CalculateSteeringMatrix() * m_chassisState.GetMatrix());
			dVector p(worldToComMatrix.TransformVector(matrix.m_posit));
			p0 = dVector(dMin(p.m_x, p0.m_x), dMin(p.m_y, p0.m_y), dMin(p.m_z, p0.m_z), 1.0f);
			p1 = dVector(dMax(p.m_x, p1.m_x), dMax(p.m_y, p1.m_y), dMax(p.m_z, p1.m_z), 1.0f);
		}

		array[0] = dVector(p0.m_x, p0.m_y, p0.m_z, 1.0f);
		array[1] = dVector(p1.m_x, p0.m_y, p0.m_z, 1.0f);
		array[2] = dVector(p1.m_x, p1.m_y, p0.m_z, 1.0f);
		array[3] = dVector(p0.m_x, p1.m_y, p0.m_z, 1.0f);
		manager->DrawSchematicCallback(this, "chassis", 0, 4, array);
	}

	{
		// draw vehicle tires
		for (dList<BodyPartTire>::dListNode* node = m_tireList.GetFirst(); node; node = node->GetNext()) {
			dMatrix matrix;
			BodyPartTire* const tire = &node->GetInfo();
			dFloat width = tire->m_data.m_width * 0.5f;
			dFloat radio = tire->m_data.m_radio;
			NewtonBody* const tireBody = tire->GetBody();
			NewtonBodyGetMatrix(tireBody, &matrix[0][0]);
			matrix.m_up = chassisMatrix.m_up;
			matrix.m_right = matrix.m_front * matrix.m_up;

			array[0] = worldToComMatrix.TransformVector(matrix.TransformVector(dVector(width, 0.0f, radio, 0.0f)));
			array[1] = worldToComMatrix.TransformVector(matrix.TransformVector(dVector(width, 0.0f, -radio, 0.0f)));
			array[2] = worldToComMatrix.TransformVector(matrix.TransformVector(dVector(-width, 0.0f, -radio, 0.0f)));
			array[3] = worldToComMatrix.TransformVector(matrix.TransformVector(dVector(-width, 0.0f, radio, 0.0f)));
			manager->DrawSchematicCallback(this, "tire", 0, 4, array);
		}
	}

	{
		// draw vehicle velocity
		dVector veloc(0.0f);
		dVector omega(0.0f);

		NewtonBodyGetOmega(chassisBody, &omega[0]);
		NewtonBodyGetVelocity(chassisBody, &veloc[0]);

		dVector localVelocity(chassisMatrix.UnrotateVector(veloc));
		localVelocity.m_y = 0.0f;

		localVelocity = projectionMatrix.RotateVector(localVelocity);
		array[0] = dVector(0.0f, 0.0f, 0.0f, 0.0f);
		array[1] = localVelocity.Scale(velocityScale);
		manager->DrawSchematicCallback(this, "velocity", 0, 2, array);

		dVector localOmega(chassisMatrix.UnrotateVector(omega));
		array[0] = dVector(0.0f, 0.0f, 0.0f, 0.0f);
		array[1] = dVector(0.0f, localOmega.m_y * 10.0f, 0.0f, 0.0f);
		manager->DrawSchematicCallback(this, "omega", 0, 2, array);
	}

	{
		dFloat scale(2.0f / (mass * 10.0f));
		// draw vehicle forces
		for (dList<BodyPartTire>::dListNode* node = m_tireList.GetFirst(); node; node = node->GetNext()) {
			BodyPartTire* const tire = &node->GetInfo();
			dMatrix matrix;
			NewtonBody* const tireBody = tire->GetBody();
			NewtonBodyGetMatrix(tireBody, &matrix[0][0]);
			matrix.m_up = chassisMatrix.m_up;
			matrix.m_right = matrix.m_front * matrix.m_up;

			dVector origin(worldToComMatrix.TransformVector(matrix.m_posit));

			dVector lateralForce(chassisMatrix.UnrotateVector(GetTireLateralForce(tire)));
			lateralForce = lateralForce.Scale(-scale);
			lateralForce = projectionMatrix.RotateVector(lateralForce);

			array[0] = origin;
			array[1] = origin + lateralForce;
			manager->DrawSchematicCallback(this, "lateralForce", 0, 2, array);

			dVector longitudinalForce(chassisMatrix.UnrotateVector(GetTireLongitudinalForce(tire)));
			longitudinalForce = longitudinalForce.Scale(-scale);
			longitudinalForce = projectionMatrix.RotateVector(longitudinalForce);

			array[0] = origin;
			array[1] = origin + longitudinalForce;
			manager->DrawSchematicCallback(this, "longitudinalForce", 0, 2, array);

			dVector veloc(0.0f);
			NewtonBodyGetVelocity(tireBody, &veloc[0]);
			veloc = chassisMatrix.UnrotateVector(veloc);
			veloc.m_y = 0.0f;
			veloc = projectionMatrix.RotateVector(veloc);
			array[0] = origin;
			array[1] = origin + veloc.Scale(velocityScale);
			manager->DrawSchematicCallback(this, "tireVelocity", 0, 2, array);
		}
	}
*/
}

void CustomVehicleController::PostUpdate(dFloat timestep, int threadIndex)
{
	dTimeTrackerEvent(__FUNCTION__);
	if (m_finalized) {
		if (!NewtonBodyGetSleepState(m_body)) {
			m_hasNewContact = false;
			if (m_isAirborned) {
				dMatrix invInertia;
				NewtonBodyGetInvInertiaMatrix(m_body, &invInertia[0][0]);
				dVector omega(invInertia.RotateVector(m_lastAngularMomentum));
				NewtonBodySetOmega(m_body, &omega[0]);
				// attenuate the angular momentum by applying a pseudo angular deficiently of drag
				m_lastAngularMomentum = m_lastAngularMomentum.Scale(0.999f);
			}

			bool hasContacts = false;
			CustomVehicleControllerManager* const manager = (CustomVehicleControllerManager*)GetManager();
			for (dList<BodyPartTire>::dListNode* tireNode = m_tireList.GetFirst(); tireNode; tireNode = tireNode->GetNext()) {
				BodyPartTire& tire = tireNode->GetInfo();
				WheelJoint* const tireJoint = (WheelJoint*)tire.GetJoint();
				tireJoint->RemoveKinematicError(timestep);
				hasContacts |= manager->Collide(&tire, threadIndex);
			}

			if (!hasContacts) {
				for (NewtonJoint* joint = NewtonBodyGetFirstContactJoint(m_body); joint; joint = NewtonBodyGetNextContactJoint(m_body, joint)) {
					hasContacts |= NewtonJointIsActive(joint) ? true : false;
				}
			}

			m_isAirborned = !hasContacts;
			if (hasContacts) {
				dMatrix inertia;
				dVector omega;
				NewtonBodyGetOmega(m_body, &omega[0]);
				NewtonBodyGetInertiaMatrix(m_body, &inertia[0][0]);
				m_lastAngularMomentum = inertia.RotateVector(omega);
			}
		}


#ifdef D_PLOT_ENGINE_CURVE 
		dFloat engineOmega = m_engine->GetRPM();
		dFloat tireTorque = m_engine->GetLeftSpiderGear()->m_tireTorque + m_engine->GetRightSpiderGear()->m_tireTorque;
		dFloat engineTorque = m_engine->GetLeftSpiderGear()->m_engineTorque + m_engine->GetRightSpiderGear()->m_engineTorque;
		fprintf(file_xxx, "%f, %f, %f,\n", engineOmega, engineTorque, m_engine->GetNominalTorque());
#endif
	}
}


void CustomVehicleController::PreUpdate(dFloat timestep, int threadIndex)
{
	dTimeTrackerEvent(__FUNCTION__);
	if (m_finalized) {
		m_chassis.ApplyDownForce ();
		ApplyLateralStabilityForces(timestep);

		if (m_brakesControl) {
			m_brakesControl->Update(timestep);
		}

		if (m_handBrakesControl) {
			m_handBrakesControl->Update(timestep);
		}

		if (m_steeringControl) {
			m_steeringControl->Update(timestep);
		}

		if (m_engineControl) {
			m_engineControl->Update(timestep);
		}

		if (ControlStateChanged()) {
			NewtonBodySetSleepState(m_body, 0);
		}
	}
}
