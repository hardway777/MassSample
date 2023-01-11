﻿// Fill out your copyright notice in the Description page of Project Settings.


#include "MSProjectileHitObservers.h"

#include "MassCommonFragments.h"
#include "MassMovementFragments.h"
#include "MSProjectileSimProcessors.h"
#include "Common/Fragments/MSFragments.h"
#include "Common/Fragments/MSOctreeFragments.h"
#include "ProjectileSim/MassProjectileHitInterface.h"
#include "MassSignalSubsystem.h"
#include "ProjectileSim/Fragments/MSProjectileFragments.h"

UMSProjectileHitObservers::UMSProjectileHitObservers()
{
	ObservedType = FHitResultFragment::StaticStruct();
	Operation = EMassObservedOperation::Add;
	ExecutionFlags = (int32)(EProcessorExecutionFlags::All);
}

void UMSProjectileHitObservers::ConfigureQueries()
{
	
	CollisionHitEventQuery.AddTagRequirement<FMSProjectileFireHitEventTag>(EMassFragmentPresence::All);
	CollisionHitEventQuery.AddRequirement<FHitResultFragment>(EMassFragmentAccess::ReadOnly);
	CollisionHitEventQuery.RegisterWithProcessor(*this);
	
	//You can always add another query for different things in the same observer processor!
	ResolveHitsQuery.AddRequirement<FMassVelocityFragment>(EMassFragmentAccess::ReadWrite);
	ResolveHitsQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadWrite);
	ResolveHitsQuery.AddRequirement<FHitResultFragment>(EMassFragmentAccess::ReadOnly);
	ResolveHitsQuery.AddTagRequirement<FMSProjectileRicochetTag>(EMassFragmentPresence::Optional);
	ResolveHitsQuery.RegisterWithProcessor(*this);



}

void UMSProjectileHitObservers::Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context)
{


			CollisionHitEventQuery.ForEachEntityChunk(EntityManager, Context, [&,this](FMassExecutionContext& Context)
			{

				auto HitResults = Context.GetFragmentView<FHitResultFragment>();

				for (int32 EntityIndex = 0; EntityIndex < Context.GetNumEntities(); ++EntityIndex)
				{
					auto Hitresult = HitResults[EntityIndex].HitResult;
							
					FMassArchetypeHandle Archetype = EntityManager.GetArchetypeForEntityUnsafe(Context.GetEntity(0));

							
					if(Hitresult.GetActor() && Hitresult.GetActor()->Implements<UMassProjectileHitInterface>())
					{
						IMassProjectileHitInterface::Execute_ProjectileHit(
							Hitresult.GetActor(),
							FMSEntityViewBPWrapper(Archetype,Context.GetEntity(EntityIndex)),
							Hitresult);
					}
				}
			});


			ResolveHitsQuery.ForEachEntityChunk(EntityManager, Context, [&,this](FMassExecutionContext& Context)
			{

				auto Transforms = Context.GetMutableFragmentView<FTransformFragment>();
				auto Velocities = Context.GetMutableFragmentView<FMassVelocityFragment>();

				auto HitResults = Context.GetFragmentView<FHitResultFragment>();


				// This is kind of a weird way to handle a "switch" Is there a better way? Manually flushing seems to cause issues.
				// Perhaps make these gamethread only as they are very sparodic?


				if(Context.DoesArchetypeHaveTag<FMSProjectileRicochetTag>())
				{
					for (int32 i = 0; i < Context.GetNumEntities(); ++i)
					{
						Context.Defer().RemoveFragment<FHitResultFragment>(Context.GetEntity(i));

						const auto& HitResult = HitResults[i].HitResult;
						auto& Transform = Transforms[i].GetMutableTransform();
						auto& Velocity = Velocities[i].Value;

						// TODO-karl this is almost certainly wrong, I have to tool around in something a bit to get a better math setup
						// Also it should be recursive at least a few times for extra bounces after the fact
						auto ReflectionLocation = FMath::GetReflectionVector((HitResult.TraceEnd - HitResult.TraceStart)*1.1f ,HitResult.ImpactNormal);
						Velocity = FMath::GetReflectionVector(Velocity ,HitResult.ImpactNormal);

						if (HitResult.PhysMaterial.IsValid())
						{
							Velocity *= HitResult.PhysMaterial.Get()->Restitution;
						}
						else
						{
							Velocity *= 0.5f;
						}
						
						// If we are too slow, we may stop here (otherwise, just consume the hitresult)
						// Magic number for now
						if(Velocity.Size() < 100.0f)
						{
							Transform.SetTranslation(HitResult.ImpactPoint);
							Context.Defer().RemoveFragment<FMassVelocityFragment>(Context.GetEntity(i));
							Context.Defer().DestroyEntity(Context.GetEntity(i));

						}
						else
						{
							Transform = FTransform(Velocity.Rotation(),ReflectionLocation+HitResult.ImpactPoint);
						}


						
					}
				}
				else
				{
					for (int32 i = 0; i < Context.GetNumEntities(); ++i)
					{
						auto& HitResult = HitResults[i].HitResult;
						FTransform& Transform = Transforms[i].GetMutableTransform();
						
						Transforms[i].GetMutableTransform().SetTranslation(HitResult.ImpactPoint);
						
						Transform.SetTranslation(HitResult.ImpactPoint);

						// todo: should probably think of a less goofy way to stop the projectile. Good enough for now?
						
						Context.Defer().RemoveFragment<FMassVelocityFragment>(Context.GetEntity(i));
						Context.Defer().DestroyEntity(Context.GetEntity(i));

					}
				}
				
			});
}


UMSEntityWasHitSignalProcessor::UMSEntityWasHitSignalProcessor()
{
	ExecutionOrder.ExecuteAfter.Add(UMSProjectileSimLineTrace::StaticClass()->GetFName());
	ExecutionOrder.ExecuteInGroup = UE::Mass::ProcessorGroupNames::Behavior;
}

void UMSEntityWasHitSignalProcessor::ConfigureQueries()
{
	// 
	EntityQuery.AddTagRequirement<FMSInOctreeGridTag>(EMassFragmentPresence::All);
	EntityQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadOnly);
}

void UMSEntityWasHitSignalProcessor::Initialize(UObject& Owner)
{
	Super::Initialize(Owner);
	UMassSignalSubsystem* SignalSubsystem = UWorld::GetSubsystem<UMassSignalSubsystem>(Owner.GetWorld());;

	SubscribeToSignal(*SignalSubsystem, MassSample::Signals::OnGetHit);

}

void UMSEntityWasHitSignalProcessor::SignalEntities(FMassEntityManager& EntityManager, FMassExecutionContext& Context, FMassSignalNameLookup& EntitySignals)
{
	
	EntityQuery.ForEachEntityChunk(EntityManager, Context, [&,this](FMassExecutionContext& Context)
	{

		auto Transforms = Context.GetFragmentView<FTransformFragment>();

		for (int32 i = 0; i < Context.GetNumEntities(); ++i)
		{
			auto Transform = Transforms[i].GetTransform();
			//DrawDebugSphere(EntityManager.GetWorld(), Transform.GetLocation(), 100.0f, 16, FColor::Blue, false, 5.0f, 0, 0.4f);
		}
	});
	
}
