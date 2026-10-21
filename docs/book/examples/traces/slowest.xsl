<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:xs="http://www.w3.org/2001/XMLSchema">
  <xsl:mode streamable="yes" use-accumulators="#all"/>
  <xsl:output method="text"/>

  <!-- How many traces have gone past. -->
  <xsl:accumulator name="count" as="xs:integer" initial-value="0"
      streamable="yes">
    <xsl:accumulator-rule match="trace" select="$value + 1"/>
  </xsl:accumulator>

  <!-- The slowest database query so far, as a map. -->
  <xsl:accumulator name="slowest" as="map(*)"
      initial-value="{ 'duration': 0 }" streamable="yes">
    <xsl:accumulator-rule match="span[@peer = 'postgresql']"
        select="if (xs:decimal(@duration) gt $value?duration)
                then { 'id': string(@id), 'duration': xs:decimal(@duration) }
                else $value"/>
  </xsl:accumulator>

  <xsl:template match="/traces">
    <xsl:apply-templates select="trace"/>
    <xsl:variable name="slowest" select="accumulator-after('slowest')"/>
    <xsl:text>{accumulator-after('count')} traces, </xsl:text>
    <xsl:text>the slowest query {$slowest?id}, {$slowest?duration} ms&#10;</xsl:text>
  </xsl:template>

  <!-- Report every 2,500th trace on the way. -->
  <xsl:template match="trace">
    <xsl:if test="accumulator-before('count') mod 2500 eq 0">
      <xsl:message>{accumulator-before('count')} traces read</xsl:message>
    </xsl:if>
  </xsl:template>
</xsl:stylesheet>
