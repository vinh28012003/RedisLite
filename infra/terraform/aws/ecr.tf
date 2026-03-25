# ecr.tf — Private container image repositories.
# Two repos: one for the Redis image, one for the failover controller.

resource "aws_ecr_repository" "redis_lite" {
  name                 = "${var.cluster_name}/redis-lite"
  image_tag_mutability = "MUTABLE"
  force_delete         = true
}

resource "aws_ecr_repository" "failover_ctl" {
  name                 = "${var.cluster_name}/failover-ctl"
  image_tag_mutability = "MUTABLE"
  force_delete         = true
}
