<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:xs="http://www.w3.org/2001/XMLSchema"
    xmlns:spec="urn:example:spec"
    exclude-result-prefixes="xs spec">
  <xsl:import href="common.xsl"/>
  <xsl:use-package name="spec.xsl" package-version="1.0"/>
  <xsl:output method="text"/>

  <xsl:template match="/">
    <!-- The first line: the series on the left, the date on the right. -->
    <xsl:variable name="date" select="rfc/front/date ! (@month || ' ' || @year)"/>
    <xsl:text>Internet-Draft{spec:spaces(72 - 14 - string-length($date))}{$date}&#10;</xsl:text>
    <xsl:apply-templates select="rfc/front/title, rfc/middle/section, rfc/back/references"/>
  </xsl:template>

  <xsl:template match="title">
    <xsl:text>&#10;{.}&#10;&#10;</xsl:text>
  </xsl:template>

  <xsl:template match="section">
    <xsl:text>{spec:number(.)}.  {name}&#10;&#10;</xsl:text>
    <xsl:apply-templates select="* except name"/>
  </xsl:template>

  <xsl:template match="references">
    <xsl:text>{name}&#10;&#10;</xsl:text>
    <xsl:apply-templates select="* except name"/>
  </xsl:template>

  <xsl:template match="t | dd">
    <xsl:call-template name="paragraph">
      <xsl:with-param name="indent" select="if (self::dd) then 6 else 3"/>
      <xsl:with-param name="text">
        <xsl:apply-templates/>
      </xsl:with-param>
    </xsl:call-template>
  </xsl:template>

  <xsl:template match="reference">
    <xsl:text>   [{@anchor}]&#10;</xsl:text>
    <xsl:call-template name="paragraph">
      <xsl:with-param name="indent" select="6"/>
      <xsl:with-param name="text">{string-join(front/author/@fullname, ', ')}, "{front/title}", {front/date/@month} {front/date/@year}, &lt;{@target}&gt;.</xsl:with-param>
    </xsl:call-template>
  </xsl:template>

  <!-- Lines of at most 72 characters, indented. -->
  <xsl:template name="paragraph">
    <xsl:param name="indent" as="xs:integer"/>
    <xsl:param name="text" as="xs:string"/>
    <xsl:variable name="spaces" select="spec:spaces($indent)"/>
    <xsl:value-of select="replace(normalize-space($text),
                                  '(.{1,' || 72 - $indent || '})( |$)',
                                  $spaces || '$1&#10;')"/>
    <xsl:text>&#10;</xsl:text>
  </xsl:template>

  <xsl:function name="spec:spaces" as="xs:string">
    <xsl:param name="count" as="xs:integer"/>
    <xsl:sequence select="string-join(replicate(' ', $count))"/>
  </xsl:function>

  <xsl:template match="dl">
    <xsl:apply-templates select="*"/>
  </xsl:template>

  <xsl:template match="dt">
    <xsl:text>   {.}&#10;</xsl:text>
  </xsl:template>

  <xsl:template match="sourcecode">
    <xsl:value-of select="tokenize(., '&#10;')[normalize-space()] ! ('     ' || . || '&#10;')"
                  separator=""/>
    <xsl:text>&#10;</xsl:text>
  </xsl:template>
</xsl:stylesheet>
