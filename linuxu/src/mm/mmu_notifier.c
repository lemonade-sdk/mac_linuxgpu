/* Classic mmu notifiers, ported from the pinned upstream mm/mmu_notifier.c
 * (__mmu_notifier_register, mmu_notifier_get_locked, mmu_notifier_put,
 * mmu_notifier_unregister, mn_hlist_release). Interval notifiers live with
 * the HMM page tracking in hmm.c, which installs linuxu_mmu_interval_release
 * so that exit also invalidates them, as upstream mn_itree_release does.
 *
 * Differences from upstream: no mm_take_all_locks (no page tables exist to
 * walk; the mmap_lock write side already excludes registration races here)
 * and the subscription lock is a pthread mutex. The invalidate_range_start/
 * end fan-out is not wired, because no linuxu path changes CPU page tables
 * of a process mm.
 */
#include <pthread.h>
#include <stdlib.h>
#include <limits.h>
#include <linux/mm.h>
#include <linux/mmu_notifier.h>
#include <linux/rcupdate.h>
#include <linux/srcu.h>
#include <linux/bug.h>

/* global SRCU for all MMs */
DEFINE_STATIC_SRCU(srcu);

struct mmu_notifier_subscriptions {
	/* all mmu notifiers registered in this mm are queued in this list */
	struct hlist_head list;
	/* to serialize the list modifications and hlist_unhashed */
	pthread_mutex_t lock;
};

void (*linuxu_mmu_interval_release)(struct mm_struct *mm);
void (*linuxu_mmu_interval_synchronize)(void);

#define hlist_for_each_entry_srcu(pos, head, member) \
	hlist_for_each_entry_rcu(pos, head, member)

/*
 * This function can't run concurrently against mmu_notifier_register
 * because mm->mm_users > 0 during mmu_notifier_register and exit_mmap
 * runs with mm_users == 0 (linuxu_process_exit calls it once no thread
 * task of the process is left). This serializes against
 * mmu_notifier_unregister with the notifier_subscriptions->lock in
 * addition to SRCU and it serializes against the other mmu notifiers with
 * SRCU.
 */
static void mn_hlist_release(struct mmu_notifier_subscriptions *subscriptions,
			     struct mm_struct *mm)
{
	struct mmu_notifier *subscription;
	int id;

	/*
	 * SRCU here will block mmu_notifier_unregister until
	 * ->release returns.
	 */
	id = srcu_read_lock(&srcu);
	hlist_for_each_entry_srcu(subscription, &subscriptions->list, hlist)
		/*
		 * If ->release runs before mmu_notifier_unregister it must be
		 * handled, as it's the only way for the driver to flush all
		 * existing sptes and stop the driver from establishing any more
		 * sptes before all the pages in the mm are freed.
		 */
		if (subscription->ops->release)
			subscription->ops->release(subscription, mm);

	pthread_mutex_lock(&subscriptions->lock);
	while (unlikely(!hlist_empty(&subscriptions->list))) {
		subscription = hlist_entry(subscriptions->list.first,
					   struct mmu_notifier, hlist);
		/*
		 * We arrived before mmu_notifier_unregister so
		 * mmu_notifier_unregister will do nothing other than to wait
		 * for ->release to finish and for mmu_notifier_unregister to
		 * return.
		 */
		hlist_del_init_rcu(&subscription->hlist);
	}
	pthread_mutex_unlock(&subscriptions->lock);
	srcu_read_unlock(&srcu, id);

	/*
	 * synchronize_srcu here prevents mmu_notifier_release from returning to
	 * exit_mmap (which would proceed with freeing all pages in the mm)
	 * until the ->release method returns, if it was invoked by
	 * mmu_notifier_unregister.
	 */
	synchronize_srcu(&srcu);
}

void __mmu_notifier_release(struct mm_struct *mm)
{
	struct mmu_notifier_subscriptions *subscriptions =
		__atomic_load_n(&mm->notifier_subscriptions, __ATOMIC_ACQUIRE);
	void (*interval_release)(struct mm_struct *) =
		__atomic_load_n(&linuxu_mmu_interval_release, __ATOMIC_ACQUIRE);

	if (interval_release)
		interval_release(mm);
	if (subscriptions && !hlist_empty(&subscriptions->list))
		mn_hlist_release(subscriptions, mm);
}

void linuxu_mm_exit(struct mm_struct *mm)
{
	if (!mm)
		return;
	if (__atomic_fetch_or(&mm->flags, 1UL << MMF_LINUXU_RELEASED,
			      __ATOMIC_ACQ_REL) & (1UL << MMF_LINUXU_RELEASED))
		return;
	__mmu_notifier_release(mm);
}

/*
 * Same as mmu_notifier_register but here the caller must hold the mmap_lock in
 * write mode.
 */
int __mmu_notifier_register(struct mmu_notifier *subscription,
			    struct mm_struct *mm)
{
	struct mmu_notifier_subscriptions *subscriptions = NULL;

	mmap_assert_write_locked(mm);
	BUG_ON(atomic_read(&mm->mm_users) <= 0);

	if (!mm->notifier_subscriptions) {
		subscriptions = calloc(1, sizeof(*subscriptions));
		if (!subscriptions)
			return -ENOMEM;
		INIT_HLIST_HEAD(&subscriptions->list);
		pthread_mutex_init(&subscriptions->lock, NULL);
		/* Unlocked readers (exit) pair with this release. */
		__atomic_store_n(&mm->notifier_subscriptions, subscriptions,
				 __ATOMIC_RELEASE);
	}

	/* Pairs with the mmdrop in mmu_notifier_unregister_* */
	mmgrab(mm);
	subscription->mm = mm;
	subscription->users = 1;

	pthread_mutex_lock(&mm->notifier_subscriptions->lock);
	hlist_add_head_rcu(&subscription->hlist,
			   &mm->notifier_subscriptions->list);
	pthread_mutex_unlock(&mm->notifier_subscriptions->lock);
	BUG_ON(atomic_read(&mm->mm_users) <= 0);
	return 0;
}

/*
 * Must not hold mmap_lock nor any other VM related lock when calling
 * this registration function. Must also ensure mm_users can't go down
 * to zero while this runs to avoid races with mmu_notifier_release.
 */
int mmu_notifier_register(struct mmu_notifier *subscription,
			  struct mm_struct *mm)
{
	int ret;

	mmap_write_lock(mm);
	ret = __mmu_notifier_register(subscription, mm);
	mmap_write_unlock(mm);
	return ret;
}

static struct mmu_notifier *
find_get_mmu_notifier(struct mm_struct *mm, const struct mmu_notifier_ops *ops)
{
	struct mmu_notifier *subscription;

	pthread_mutex_lock(&mm->notifier_subscriptions->lock);
	hlist_for_each_entry_srcu(subscription,
				  &mm->notifier_subscriptions->list, hlist) {
		if (subscription->ops != ops)
			continue;

		if (likely(subscription->users != UINT_MAX))
			subscription->users++;
		else
			subscription = ERR_PTR(-EOVERFLOW);
		pthread_mutex_unlock(&mm->notifier_subscriptions->lock);
		return subscription;
	}
	pthread_mutex_unlock(&mm->notifier_subscriptions->lock);
	return NULL;
}

/*
 * mmu_notifier_get_locked - Return the single struct mmu_notifier for
 *                           the mm & ops
 *
 * This function either allocates a new mmu_notifier via
 * ops->alloc_notifier(), or returns an already existing notifier on the
 * list. The value of the ops pointer is used to determine when two notifiers
 * are the same. Each call must be paired with mmu_notifier_put(). The caller
 * must hold the write side of mm->mmap_lock.
 */
struct mmu_notifier *mmu_notifier_get_locked(const struct mmu_notifier_ops *ops,
					     struct mm_struct *mm)
{
	struct mmu_notifier *subscription;
	int ret;

	mmap_assert_write_locked(mm);

	if (mm->notifier_subscriptions) {
		subscription = find_get_mmu_notifier(mm, ops);
		if (subscription)
			return subscription;
	}

	subscription = ops->alloc_notifier(mm);
	if (IS_ERR(subscription))
		return subscription;
	subscription->ops = ops;
	ret = __mmu_notifier_register(subscription, mm);
	if (ret)
		goto out_free;
	return subscription;
out_free:
	subscription->ops->free_notifier(subscription);
	return ERR_PTR(ret);
}

struct mmu_notifier *mmu_notifier_get(const struct mmu_notifier_ops *ops,
				      struct mm_struct *mm)
{
	struct mmu_notifier *ret;

	mmap_write_lock(mm);
	ret = mmu_notifier_get_locked(ops, mm);
	mmap_write_unlock(mm);
	return ret;
}

/* this is called after the last mmu_notifier_unregister() returned */
void __mmu_notifier_subscriptions_destroy(struct mm_struct *mm)
{
	struct mmu_notifier_subscriptions *subscriptions = mm->notifier_subscriptions;

	BUG_ON(!hlist_empty(&subscriptions->list));
	pthread_mutex_destroy(&subscriptions->lock);
	free(subscriptions);
	mm->notifier_subscriptions = NULL;
}

/*
 * This releases the mm_count pin automatically and frees the mm
 * structure if it was the last user of it. It serializes against
 * running mmu notifiers with SRCU and against mmu_notifier_unregister
 * with the unregister lock + SRCU.
 */
void mmu_notifier_unregister(struct mmu_notifier *subscription,
			     struct mm_struct *mm)
{
	BUG_ON(atomic_read(&mm->mm_count) <= 0);

	if (!hlist_unhashed(&subscription->hlist)) {
		/*
		 * SRCU here will force exit_mmap to wait for ->release to
		 * finish before freeing the pages.
		 */
		int id;

		id = srcu_read_lock(&srcu);
		/*
		 * exit_mmap will block in mmu_notifier_release to guarantee
		 * that ->release is called before freeing the pages.
		 */
		if (subscription->ops->release)
			subscription->ops->release(subscription, mm);
		srcu_read_unlock(&srcu, id);

		pthread_mutex_lock(&mm->notifier_subscriptions->lock);
		/*
		 * Can not use list_del_rcu() since __mmu_notifier_release
		 * can delete it before we hold the lock.
		 */
		hlist_del_init_rcu(&subscription->hlist);
		pthread_mutex_unlock(&mm->notifier_subscriptions->lock);
	}

	/*
	 * Wait for any running method to finish, of course including
	 * ->release if it was run by mmu_notifier_release instead of us.
	 */
	synchronize_srcu(&srcu);

	BUG_ON(atomic_read(&mm->mm_count) <= 0);

	mmdrop(mm);
}

static void mmu_notifier_free_rcu(struct rcu_head *rcu)
{
	struct mmu_notifier *subscription =
		container_of(rcu, struct mmu_notifier, rcu);
	struct mm_struct *mm = subscription->mm;

	subscription->ops->free_notifier(subscription);
	/* Pairs with the get in __mmu_notifier_register() */
	mmdrop(mm);
}

/*
 * mmu_notifier_put - Release the reference on the notifier
 *
 * Paired with each mmu_notifier_get(). If this is the last reference then
 * the notifier is freed asynchronously after an SRCU grace period. Unlike
 * mmu_notifier_unregister() the get/put flow only calls ops->release when
 * the mm_struct is destroyed; free_notifier is always called. This can be
 * called from the ops->release callback.
 */
void mmu_notifier_put(struct mmu_notifier *subscription)
{
	struct mm_struct *mm = subscription->mm;

	pthread_mutex_lock(&mm->notifier_subscriptions->lock);
	if (WARN_ON(!subscription->users) || --subscription->users)
		goto out_unlock;
	hlist_del_init_rcu(&subscription->hlist);
	pthread_mutex_unlock(&mm->notifier_subscriptions->lock);

	call_srcu(&srcu, &subscription->rcu, mmu_notifier_free_rcu);
	return;

out_unlock:
	pthread_mutex_unlock(&mm->notifier_subscriptions->lock);
}

/*
 * mmu_notifier_synchronize - Ensure all mmu_notifiers are freed
 *
 * This function ensures that all outstanding async SRU work from
 * mmu_notifier_put() is completed and interval notifier invalidations
 * (hmm.c) have finished.
 */
void mmu_notifier_synchronize(void)
{
	void (*interval_synchronize)(void) =
		__atomic_load_n(&linuxu_mmu_interval_synchronize, __ATOMIC_ACQUIRE);

	srcu_barrier(&srcu);
	if (interval_synchronize)
		interval_synchronize();
}
