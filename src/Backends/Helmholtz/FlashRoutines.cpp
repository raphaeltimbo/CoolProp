#include "VLERoutines.h"
#include "FlashRoutines.h"
#include "HelmholtzEOSMixtureBackend.h"

namespace CoolProp{

void FlashRoutines::PT_flash(HelmholtzEOSMixtureBackend &HEOS)
{
	if (HEOS.imposed_phase_index == iphase_not_imposed) // If no phase index is imposed (see set_components function)
	{
		// Find the phase, while updating all internal variables possible
		HEOS.T_phase_determination_pure_or_pseudopure(iP, HEOS._p);
		
		// Check if twophase solution
		if (!HEOS.isHomogeneousPhase())
		{
			throw ValueError("twophase not implemented yet");
		}
	}
	
	// Find density
	HEOS._rhomolar = HEOS.solver_rho_Tp(HEOS._T, HEOS._p);
	HEOS._Q = -1;
}

void FlashRoutines::QT_flash(HelmholtzEOSMixtureBackend &HEOS)
{
    long double T = HEOS._T;
    if (HEOS.is_pure_or_pseudopure)
    {
		// The maximum possible saturation temperature
		// Critical point for pure fluids, slightly different for pseudo-pure, very different for mixtures
		long double Tmax_sat = HEOS.calc_Tmax_sat();
		
		// Check what the minimum limits for the equation of state are
		long double Tmin_satL, Tmin_satV, Tmin_sat;
		HEOS.calc_Tmin_sat(Tmin_satL, Tmin_satV);
		Tmin_sat = std::max(Tmin_satL, Tmin_satV);
		
		// Check limits
		if (!is_in_closed_range(Tmin_sat, Tmax_sat, static_cast<long double>(HEOS._T))){
			throw ValueError(format("Temperature to QT_flash [%6g K] must be in range [%8g K, %8g K]",HEOS._T, Tmin_sat, Tmax_sat));
		}
        
		if (!(HEOS.components[0]->pEOS->pseudo_pure))
        {
            // Set some imput options
            SaturationSolvers::saturation_T_pure_options options;
            options.use_guesses = false;
			double increment = 0.2;
            try{
                for (double omega = 1.0; omega > 0; omega -= increment){
                    try{
                        options.omega = omega;
                        
                        // Actually call the solver
                        SaturationSolvers::saturation_T_pure(&HEOS, HEOS._T, options);
                        
                        // If you get here, there was no error, all is well
                        break;
                    }
                    catch(std::exception &){
                        if (omega < 1.1*increment){
                            throw;
                        }
                        // else we are going to try again with a smaller omega
                    }
                }
            }
            catch(std::exception &){
                try{
                    // We may need to polish the solution at low pressure
                    SaturationSolvers::saturation_T_pure_1D_P(&HEOS, T, options);
                }
                catch(std::exception &){
                    SaturationSolvers::saturation_critical(&HEOS, iT, T);
                }
            }
            // Load the outputs
            HEOS._phase = iphase_twophase;
            HEOS._p = HEOS._Q*HEOS.SatV->p() + (1- HEOS._Q)*HEOS.SatL->p();
            HEOS._rhomolar = 1/(HEOS._Q/HEOS.SatV->rhomolar() + (1 - HEOS._Q)/HEOS.SatL->rhomolar());
        }
        else{
            // Pseudo-pure fluid
            long double rhoLanc, rhoVanc, rhoLsat, rhoVsat;
            long double psatLanc = HEOS.components[0]->ancillaries.pL.evaluate(HEOS._T); // These ancillaries are used explicitly
            long double psatVanc = HEOS.components[0]->ancillaries.pV.evaluate(HEOS._T); // These ancillaries are used explicitly
            try{
                rhoLanc = HEOS.components[0]->ancillaries.rhoL.evaluate(HEOS._T);
                rhoVanc = HEOS.components[0]->ancillaries.rhoV.evaluate(HEOS._T);

                if (!ValidNumber(rhoLanc) || !ValidNumber(rhoVanc))
                {
                    throw ValueError("pseudo-pure failed");
                }

                rhoLsat = HEOS.solver_rho_Tp(HEOS._T, psatLanc, rhoLanc);
                rhoVsat = HEOS.solver_rho_Tp(HEOS._T, psatVanc, rhoVanc);
                if (!ValidNumber(rhoLsat) || !ValidNumber(rhoVsat) ||
                     fabs(rhoLsat/rhoLanc-1) > 0.5 || fabs(rhoVanc/rhoVsat-1) > 0.5)
                {
                    throw ValueError("pseudo-pure failed");
                }
            }
            catch (std::exception &){
                // Near the critical point, the behavior is not very nice, so we will just use the ancillary near the critical point
                rhoLsat = rhoLanc;
                rhoVsat = rhoVanc;
            }
            HEOS._phase = iphase_twophase;
            HEOS._p = HEOS._Q*psatVanc + (1-HEOS._Q)*psatLanc;
            HEOS._rhomolar = 1/(HEOS._Q/rhoVsat + (1-HEOS._Q)/rhoLsat);
            HEOS.SatL->update(DmolarT_INPUTS, rhoLsat, HEOS._T);
            HEOS.SatV->update(DmolarT_INPUTS, rhoVsat, HEOS._T);
        }
    }
    else
    {
        // Set some input options
        SaturationSolvers::mixture_VLE_IO options;
        options.sstype = SaturationSolvers::imposed_T;
        options.Nstep_max = 20;

        // Get an extremely rough guess by interpolation of ln(p) v. T curve where the limits are mole-fraction-weighted
        long double pguess = SaturationSolvers::saturation_preconditioner(&HEOS, HEOS._T, SaturationSolvers::imposed_T, HEOS.mole_fractions);

        // Use Wilson iteration to obtain updated guess for pressure
        pguess = SaturationSolvers::saturation_Wilson(&HEOS, HEOS._Q, HEOS._T, SaturationSolvers::imposed_T, HEOS.mole_fractions, pguess);

        // Actually call the successive substitution solver
        SaturationSolvers::successive_substitution(HEOS, HEOS._Q, HEOS._T, pguess, HEOS.mole_fractions, HEOS.K, options);

        HEOS._p = options.p;
        HEOS._rhomolar = 1/(HEOS._Q/options.rhomolar_vap+(1-HEOS._Q)/options.rhomolar_liq);
    }
}
void FlashRoutines::PQ_flash(HelmholtzEOSMixtureBackend &HEOS)
{
    if (HEOS.is_pure_or_pseudopure)
    {
        if (HEOS.components[0]->pEOS->pseudo_pure){
            // It is a psedo-pure mixture
            throw NotImplementedError("PQ_flash not implemented for pseudo-pure fluids yet");
        }
        else{
            // Critical point for pure fluids, slightly different for pseudo-pure, very different for mixtures
            long double pmax_sat = HEOS.calc_pmax_sat();
            
            // Check what the minimum limits for the equation of state are
            long double pmin_satL, pmin_satV, pmin_sat;
            HEOS.calc_pmin_sat(pmin_satL, pmin_satV);
            pmin_sat = std::max(pmin_satL, pmin_satV);
            
            // Check limits
            if (!is_in_closed_range(pmin_sat*0.999999, pmax_sat*1.000001, static_cast<long double>(HEOS._p))){
                throw ValueError(format("Pressure to PQ_flash [%6g Pa] must be in range [%8Lg Pa, %8Lg Pa]",HEOS._p, pmin_sat, pmax_sat));
            }
            // ------------------
            // It is a pure fluid
            // ------------------

            // Set some imput options
            SaturationSolvers::saturation_PHSU_pure_options options;
            // Specified variable is pressure
            options.specified_variable = SaturationSolvers::saturation_PHSU_pure_options::IMPOSED_PL;
            // Use logarithm of delta as independent variables
            options.use_logdelta = false;
			
			double increment = 0.4;

            try{
                for (double omega = 1.0; omega > 0; omega -= increment){
                    try{
                        options.omega = omega;
                        
                        // Actually call the solver
                        SaturationSolvers::saturation_PHSU_pure(&HEOS, HEOS._p, options);
                        
                        // If you get here, there was no error, all is well
                        break;
                    }
                    catch(std::exception &e){
                        if (omega < 1.1*increment){
                            throw;
                        }
                        // else we are going to try again with a smaller omega
                    }
                }
            }
            catch(std::exception &){
                // We may need to polish the solution at low pressure
                SaturationSolvers::saturation_P_pure_1D_T(&HEOS, HEOS._p, options);
            }

            // Load the outputs
            HEOS._phase = iphase_twophase;
            HEOS._p = HEOS._Q*HEOS.SatV->p() + (1 - HEOS._Q)*HEOS.SatL->p();
            HEOS._rhomolar = 1/(HEOS._Q/HEOS.SatV->rhomolar() + (1 - HEOS._Q)/HEOS.SatL->rhomolar());
            HEOS._T = HEOS.SatL->T();
        }
    }
    else
    {

        double pc = HEOS.components[0]->pEOS->reduce.p;
        double Tc = HEOS.components[0]->pEOS->reduce.T;
        double Tt = HEOS.components[0]->pEOS->Ttriple;
        double pt = HEOS.components[0]->pEOS->ptriple;
        double Tsat_guess = 1/(1/Tc-(1/Tt-1/Tc)/log(pc/pt)*log(HEOS._p/pc));

        // Set some imput options
        SaturationSolvers::mixture_VLE_IO io;
        io.sstype = SaturationSolvers::imposed_p;
        io.Nstep_max = 20;

        // Get an extremely rough guess by interpolation of ln(p) v. T curve where the limits are mole-fraction-weighted
        long double Tguess = SaturationSolvers::saturation_preconditioner(&HEOS, HEOS._p, SaturationSolvers::imposed_p, HEOS.mole_fractions);

        // Use Wilson iteration to obtain updated guess for temperature
        Tguess = SaturationSolvers::saturation_Wilson(&HEOS, HEOS._Q, HEOS._p, SaturationSolvers::imposed_p, HEOS.mole_fractions, Tguess);

        // Actually call the successive substitution solver
        SaturationSolvers::successive_substitution(HEOS, HEOS._Q, Tguess, HEOS._p, HEOS.mole_fractions, HEOS.K, io);

        // Load the outputs
        HEOS._phase = iphase_twophase;
        HEOS._p = HEOS.SatV->p();
        HEOS._rhomolar = 1/(HEOS._Q/HEOS.SatV->rhomolar() + (1 - HEOS._Q)/HEOS.SatL->rhomolar());
        HEOS._T = HEOS.SatL->T();

        //PhaseEnvelope::PhaseEnvelope_GV ENV_GV;
        //ENV_GV.build(&HEOS, HEOS.mole_fractions, HEOS.K, io);
    }
}
// D given and one of P,H,S,U
void FlashRoutines::PHSU_D_flash(HelmholtzEOSMixtureBackend &HEOS, int other)
{
    // Define the residual to be driven to zero
    class solver_resid : public FuncWrapper1D
    {
    public:

        HelmholtzEOSMixtureBackend *HEOS;
        long double r, eos, rhomolar, value, T;
        int other;

        solver_resid(HelmholtzEOSMixtureBackend *HEOS, long double rhomolar, long double value, int other) : HEOS(HEOS), rhomolar(rhomolar), value(value), other(other){};
        double call(double T){
            this->T = T;
            switch(other)
            {
            case iP:
                eos = HEOS->calc_pressure_nocache(T, rhomolar); break;
            case iSmolar:
                eos = HEOS->calc_smolar_nocache(T, rhomolar); break;
            case iHmolar:
                eos = HEOS->calc_hmolar_nocache(T, rhomolar); break;
            case iUmolar:
                eos = HEOS->calc_umolar_nocache(T, rhomolar); break;
            default:
                throw ValueError(format("Input not supported"));
            }

            r = eos - value;
            return r;
        };
    };

    std::string errstring;

    if (HEOS.imposed_phase_index != iphase_not_imposed)
    {
        // Use the phase defined by the imposed phase
        HEOS._phase = HEOS.imposed_phase_index;
    }
    else
    {
        if (HEOS.is_pure_or_pseudopure)
        {
            CoolPropFluid * component = HEOS.components[0];

            shared_ptr<HelmholtzEOSMixtureBackend> Sat;
            long double rhoLtriple = component->triple_liquid.rhomolar;
            long double rhoVtriple = component->triple_vapor.rhomolar;
            // Check if in the "normal" region
            if (HEOS._rhomolar >= rhoVtriple && HEOS._rhomolar <= rhoLtriple)
            {
                long double yL, yV, value, y_solid;
                long double TLtriple = component->triple_liquid.T; ///TODO: separate TL and TV for ppure
                long double TVtriple = component->triple_vapor.T;

                // First check if solid (below the line connecting the triple point values) - this is an error for now
                switch (other)
                {
                    case iSmolar:
                        yL = HEOS.calc_smolar_nocache(TLtriple, rhoLtriple); yV = HEOS.calc_smolar_nocache(TVtriple, rhoVtriple); value = HEOS._smolar; break;
                    case iHmolar:
                        yL = HEOS.calc_hmolar_nocache(TLtriple, rhoLtriple); yV = HEOS.calc_hmolar_nocache(TVtriple, rhoVtriple); value = HEOS._hmolar; break;
                    case iUmolar:
                        yL = HEOS.calc_umolar_nocache(TLtriple, rhoLtriple); yV = HEOS.calc_umolar_nocache(TVtriple, rhoVtriple); value = HEOS._umolar; break;
                    case iP:
                        yL = HEOS.calc_pressure_nocache(TLtriple, rhoLtriple); yV = HEOS.calc_pressure_nocache(TVtriple, rhoVtriple); value = HEOS._p; break;
                    default:
                        throw ValueError(format("Input is invalid"));
                }
                y_solid = (yV-yL)/(1/rhoVtriple-1/rhoLtriple)*(1/HEOS._rhomolar-1/rhoLtriple) + yL;

                if (value < y_solid){ throw ValueError(format("Other input [%d:%g] is solid", other, value));}

                // Check if other is above the saturation value.
                SaturationSolvers::saturation_D_pure_options options;
                options.omega = 1;
                options.use_logdelta = false;
                if (HEOS._rhomolar > HEOS._crit.rhomolar)
                {
                    options.imposed_rho = SaturationSolvers::saturation_D_pure_options::IMPOSED_RHOL;
                    SaturationSolvers::saturation_D_pure(&HEOS, HEOS._rhomolar, options);
                    // SatL and SatV have the saturation values
                    Sat = HEOS.SatL;
                }
                else
                {
                    options.imposed_rho = SaturationSolvers::saturation_D_pure_options::IMPOSED_RHOV;
                    SaturationSolvers::saturation_D_pure(&HEOS, HEOS._rhomolar, options);
                    // SatL and SatV have the saturation values
                    Sat = HEOS.SatV;
                }

                // If it is above, it is not two-phase and either liquid, vapor or supercritical
                if (value > Sat->keyed_output(other))
                {
                    solver_resid resid(&HEOS, HEOS._rhomolar, value, other);
                    HEOS._phase = iphase_twophase;
                    HEOS._T = Brent(resid, Sat->keyed_output(iT), HEOS.Tmax()+1, DBL_EPSILON, 1e-12, 100, errstring);
                    HEOS._Q = 10000;
                    HEOS.calc_pressure();
                }
                else
                {
                    throw NotImplementedError("Two-phase for PHSU_D_flash not supported yet");
                }

            }
            // Check if vapor/solid region below triple point vapor density
            else if (HEOS._rhomolar < component->triple_vapor.rhomolar)
            {
                long double y, value;
                long double TVtriple = component->triple_vapor.T; //TODO: separate TL and TV for ppure

                // If value is above the value calculated from X(Ttriple, _rhomolar), it is vapor
                switch (other)
                {
                    case iSmolar:
                        y = HEOS.calc_smolar_nocache(TVtriple, HEOS._rhomolar); value = HEOS._smolar; break;
                    case iHmolar:
                        y = HEOS.calc_hmolar_nocache(TVtriple, HEOS._rhomolar); value = HEOS._hmolar; break;
                    case iUmolar:
                        y = HEOS.calc_umolar_nocache(TVtriple, HEOS._rhomolar); value = HEOS._umolar; break;
                    case iP:
                        y = HEOS.calc_pressure_nocache(TVtriple, HEOS._rhomolar); value = HEOS._p; break;
                    default:
                        throw ValueError(format("Input is invalid"));
                }
                if (value > y)
                {
                    solver_resid resid(&HEOS, HEOS._rhomolar, value, other);
                    HEOS._phase = iphase_gas;
                    HEOS._T = Brent(resid, TVtriple, HEOS.Tmax()+1, DBL_EPSILON, 1e-12, 100, errstring);
                    HEOS._Q = 10000;
                    HEOS.calc_pressure();
                }
                else
                {
                    throw ValueError(format("D < DLtriple %g %g", value, y));
                }

            }
            // Check in the liquid/solid region above the triple point density
            else
            {
                long double y, value;
                long double TLtriple = component->pEOS->Ttriple;

                // If value is above the value calculated from X(Ttriple, _rhomolar), it is vapor
                switch (other)
                {
                    case iSmolar:
                        y = HEOS.calc_smolar_nocache(TLtriple, HEOS._rhomolar); value = HEOS._smolar; break;
                    case iHmolar:
                        y = HEOS.calc_hmolar_nocache(TLtriple, HEOS._rhomolar); value = HEOS._hmolar; break;
                    case iUmolar:
                        y = HEOS.calc_umolar_nocache(TLtriple, HEOS._rhomolar); value = HEOS._umolar; break;
                    case iP:
                        y = HEOS.calc_pressure_nocache(TLtriple, HEOS._rhomolar); value = HEOS._p; break;
                    default:
                        throw ValueError(format("Input is invalid"));
                }
                if (value > y)
                {
                    solver_resid resid(&HEOS, HEOS._rhomolar, value, other);
                    HEOS._phase = iphase_liquid;
                    HEOS._T = Brent(resid, TLtriple, HEOS.Tmax()+1, DBL_EPSILON, 1e-12, 100, errstring);
                    HEOS._Q = 10000;
                    HEOS.calc_pressure();
                }
                else
                {
                    throw ValueError(format("D < DLtriple %g %g", value, y));
                }
            }
        }
        else
            throw NotImplementedError("PHSU_D_flash not ready for mixtures");
    }
}

void FlashRoutines::HSU_P_flash_singlephase_Newton(HelmholtzEOSMixtureBackend &HEOS, int other, long double T0, long double rhomolar0)
{
    double A[2][2], B[2][2];
    long double y;
    HelmholtzEOSMixtureBackend _HEOS(HEOS.get_components());
    _HEOS.update(DmolarT_INPUTS, rhomolar0, T0);
    long double Tc = HEOS.calc_T_critical();
    long double rhoc = HEOS.calc_rhomolar_critical();
    long double R = HEOS.gas_constant();
    long double p = HEOS.p();
    switch (other)
    {
        case iHmolar: y = HEOS.hmolar(); break;
        case iSmolar: y = HEOS.smolar(); break;
    }
    
    long double worst_error = 999;
    int iter = 0;
    bool failed = false;
    long double omega = 1.0, f2, df2_dtau, df2_ddelta;
    long double tau = _HEOS.tau(), delta = _HEOS.delta();
    while (worst_error>1e-6 && failed == false)
    {
        
        // All the required partial derivatives
        long double a0 = _HEOS.calc_alpha0_deriv_nocache(0,0,HEOS.mole_fractions, tau, delta,Tc,rhoc);
        long double da0_ddelta = _HEOS.calc_alpha0_deriv_nocache(0,1,HEOS.mole_fractions, tau, delta,Tc,rhoc);
        long double da0_dtau = _HEOS.calc_alpha0_deriv_nocache(1,0,HEOS.mole_fractions, tau, delta,Tc,rhoc);
        long double d2a0_dtau2 = _HEOS.calc_alpha0_deriv_nocache(2,0,HEOS.mole_fractions, tau, delta,Tc,rhoc);
        long double d2a0_ddelta_dtau = 0.0;
        
        long double ar = _HEOS.calc_alphar_deriv_nocache(0,0,HEOS.mole_fractions, tau, delta);
        long double dar_dtau = _HEOS.calc_alphar_deriv_nocache(1,0,HEOS.mole_fractions, tau, delta);
        long double dar_ddelta = _HEOS.calc_alphar_deriv_nocache(0,1,HEOS.mole_fractions, tau, delta);
        long double d2ar_ddelta_dtau = _HEOS.calc_alphar_deriv_nocache(1,1,HEOS.mole_fractions, tau, delta);
        long double d2ar_ddelta2 = _HEOS.calc_alphar_deriv_nocache(0,2,HEOS.mole_fractions, tau, delta);
        long double d2ar_dtau2 = _HEOS.calc_alphar_deriv_nocache(2,0,HEOS.mole_fractions, tau, delta);

        long double f1 = delta/tau*(1+delta*dar_ddelta)-p/(rhoc*R*Tc);
        long double df1_dtau = (1+delta*dar_ddelta)*(-delta/tau/tau)+delta/tau*(delta*d2ar_ddelta_dtau);
        long double df1_ddelta = (1.0/tau)*(1+2.0*delta*dar_ddelta+delta*delta*d2ar_ddelta2);
        switch (other)
        {
            case iHmolar:
            {
                f2 = (1+delta*dar_ddelta)+tau*(da0_dtau+dar_dtau) - tau*y/(R*Tc);
                df2_dtau = delta*d2ar_ddelta_dtau+da0_dtau+dar_dtau+tau*(d2a0_dtau2+d2ar_dtau2) - y/(R*Tc);
                df2_ddelta = (dar_ddelta+delta*d2ar_ddelta2)+tau*(d2a0_ddelta_dtau+d2ar_ddelta_dtau);    
                break;
            }
            case iSmolar:
            {
                f2 = tau*(da0_dtau+dar_dtau)-ar-a0-y/R;
                df2_dtau = tau*(d2a0_dtau2 + d2ar_dtau2)+(da0_dtau+dar_dtau)-dar_dtau-da0_dtau;
                df2_ddelta = tau*(d2a0_ddelta_dtau+d2ar_ddelta_dtau)-dar_ddelta-da0_ddelta;
                break;
            }
            default:
                throw ValueError("other variable in HSU_P_flash_singlephase_Newton is invalid");
        }

        //First index is the row, second index is the column
        A[0][0]=df1_dtau;
        A[0][1]=df1_ddelta;
        A[1][0]=df2_dtau;
        A[1][1]=df2_ddelta;

        //double det = A[0][0]*A[1][1]-A[1][0]*A[0][1];

        MatInv_2(A,B);
        tau -= omega*(B[0][0]*f1+B[0][1]*f2);
        delta -= omega*(B[1][0]*f1+B[1][1]*f2);

        if (fabs(f1)>fabs(f2))
            worst_error=fabs(f1);
        else
            worst_error=fabs(f2);

        if (!ValidNumber(f1) || !ValidNumber(f2))
        {
            throw SolutionError(format("Invalid values for inputs p=%g y=%g for fluid %s in HSU_P_flash_singlephase", p, y, _HEOS.name().c_str()));
        }

        iter += 1;
        if (iter>100)
        {
            throw SolutionError(format("HSU_P_flash_singlephase did not converge with inputs p=%g h=%g for fluid %s", p, y,_HEOS.name().c_str()));
        }
    }
    
    HEOS.update(DmolarT_INPUTS, rhoc*delta, Tc/tau);
}
void FlashRoutines::HSU_P_flash_singlephase_Brent(HelmholtzEOSMixtureBackend &HEOS, int other, long double value, long double Tmin, long double Tmax)
{
    if (!ValidNumber(HEOS._p)){throw ValueError("value for p in HSU_P_flash_singlephase_Brent is invalid");};
    if (!ValidNumber(value)){throw ValueError("value for other in HSU_P_flash_singlephase_Brent is invalid");};
	class solver_resid : public FuncWrapper1D
    {
    public:

        HelmholtzEOSMixtureBackend *HEOS;
        long double r, eos, p, value, T, rhomolar;
        int other;
        int iter;
        long double r0, r1, T1, T0, pp;
        solver_resid(HelmholtzEOSMixtureBackend *HEOS, long double p, long double value, int other) : 
                HEOS(HEOS), p(p), value(value), other(other)
                {
                    iter = 0;
                    // Specify the state to avoid saturation calls
                    HEOS->specify_phase(HEOS->phase());
                };
        double call(double T){

			this->T = T;

			// Run the solver with T,P as inputs;
			HEOS->update(PT_INPUTS, p, T);
			
            rhomolar = HEOS->rhomolar();
            HEOS->update(DmolarT_INPUTS, rhomolar, T);
			// Get the value of the desired variable
			eos = HEOS->keyed_output(other);
            pp = HEOS->p();
            

			// Difference between the two is to be driven to zero
            r = eos - value;
			
            // Store values for later use if there are errors
            if (iter == 0){ 
                r0 = r; T0 = T; 
            }
            else if (iter == 1){
                r1 = r; T1 = T; 
            }
            else{
                r0 = r1; T0 = T1;
                r1 = r;  T1 = T;
            }

            iter++;
            return r;
        };
    };
	solver_resid resid(&HEOS, HEOS._p, value, other);
	
	std::string errstr;
	double T = Brent(resid, Tmin, Tmax, DBL_EPSILON, 1e-12, 100, errstr);

    // Un-impose the phase of the fluid
    HEOS.specify_phase(iphase_not_imposed);
    int rr = 4;
}

// P given and one of H, S, or U
void FlashRoutines::HSU_P_flash(HelmholtzEOSMixtureBackend &HEOS, int other)
{
    bool saturation_called = false;
    long double value;
    if (HEOS.imposed_phase_index != iphase_not_imposed)
    {
        // Use the phase defined by the imposed phase
        HEOS._phase = HEOS.imposed_phase_index;
    }
    else
    {
        if (HEOS.is_pure_or_pseudopure)
        {

            // Find the phase, while updating all internal variables possible
            switch (other)
            {
                case iSmolar:
                    value = HEOS.smolar(); HEOS.p_phase_determination_pure_or_pseudopure(iSmolar, value, saturation_called); break;
                case iHmolar:
                    value = HEOS.hmolar(); HEOS.p_phase_determination_pure_or_pseudopure(iHmolar, value, saturation_called); break;
                case iUmolar:
                    value = HEOS.umolar(); HEOS.p_phase_determination_pure_or_pseudopure(iUmolar, value, saturation_called); break;
                default:
                    throw ValueError(format("Input for other [%s] is invalid", get_parameter_information(other, "long").c_str()));
            }
			if (HEOS.isHomogeneousPhase())
			{
                // Now we use the single-phase solver to find T,rho given P,Y using a 
                // bounded 1D solver by adjusting T and using given value of p
                long double Tmin, Tmax;
                switch(HEOS._phase)
                {
                    case iphase_gas:
                    {
                        Tmax = HEOS.Tmax()+1;
                        if (saturation_called){ Tmin = HEOS.SatV->T();}else{Tmin = HEOS._TVanc.pt();}
                        break;
                    }
                    case iphase_liquid:
                    {
                        if (saturation_called){ Tmax = HEOS.SatL->T();}else{Tmax = HEOS._TLanc.pt();}
						
                        // Sometimes the minimum pressure for the melting line is a bit above the triple point pressure
						if (HEOS.has_melting_line() && HEOS._p > HEOS.calc_melting_line(iP_min, -1, -1)){
                            Tmin = HEOS.calc_melting_line(iT, iP, HEOS._p)-1e-3;
						}
						else{
							Tmin = HEOS.Tmin()-1e-3;
						}
                        break;
                    }
                    case iphase_supercritical_liquid:
                    case iphase_supercritical_gas:
                    case iphase_supercritical:
                    {
                        Tmax = HEOS.Tmax()+1;
                        // Sometimes the minimum pressure for the melting line is a bit above the triple point pressure
                        if (HEOS.has_melting_line() && HEOS._p > HEOS.calc_melting_line(iP_min, -1, -1)){
							Tmin = HEOS.calc_melting_line(iT, iP, HEOS._p)-1e-3;
						}
						else{
							Tmin = HEOS.Tmin()-1e-3;
						}
                        break;
                    }
                    default:
                    { throw ValueError(format("Not a valid homogeneous state")); }
                }
				HSU_P_flash_singlephase_Brent(HEOS, other, value, Tmin, Tmax);
				HEOS._Q = -1;
			}
        }
        else
        {
            throw NotImplementedError("HSU_P_flash does not support mixtures (yet)");
        }
    }    
}
void FlashRoutines::DHSU_T_flash(HelmholtzEOSMixtureBackend &HEOS, int other)
{
    if (HEOS.imposed_phase_index != iphase_not_imposed)
    {
        // Use the phase defined by the imposed phase
        HEOS._phase = HEOS.imposed_phase_index;
    }
    else
    {
        if (HEOS.is_pure_or_pseudopure)
        {
            // Find the phase, while updating all internal variables possible
            switch (other)
            {
                case iDmolar:
                    HEOS.T_phase_determination_pure_or_pseudopure(iDmolar, HEOS._rhomolar); break;
                case iSmolar:
                    HEOS.T_phase_determination_pure_or_pseudopure(iSmolar, HEOS._smolar); break;
                case iHmolar:
                    HEOS.T_phase_determination_pure_or_pseudopure(iHmolar, HEOS._hmolar); break;
                case iUmolar:
                    HEOS.T_phase_determination_pure_or_pseudopure(iUmolar, HEOS._umolar); break;
                default:
                    throw ValueError(format("Input is invalid"));
            }
        }
        else
        {
            HEOS._phase = iphase_gas;
            throw NotImplementedError("DHSU_T_flash does not support mixtures (yet)");
            // Find the phase, while updating all internal variables possible
        }
    }

    if (HEOS.isHomogeneousPhase() && !ValidNumber(HEOS._p))
    {
        switch (other)
        {
            case iDmolar:
                break;
            case iHmolar:
                HEOS._rhomolar = HEOS.solver_for_rho_given_T_oneof_HSU(HEOS._T, HEOS._hmolar, iHmolar); break;
            case iSmolar:
                HEOS._rhomolar = HEOS.solver_for_rho_given_T_oneof_HSU(HEOS._T, HEOS._smolar, iSmolar); break;
            case iUmolar:
                HEOS._rhomolar = HEOS.solver_for_rho_given_T_oneof_HSU(HEOS._T, HEOS._umolar, iUmolar); break;
            default:
                break;
        }
        HEOS.calc_pressure();
        HEOS._Q = -1;
    }
}

} /* namespace CoolProp */
